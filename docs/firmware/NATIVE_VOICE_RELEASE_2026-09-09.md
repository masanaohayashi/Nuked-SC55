# ボイスのリリース準備

[解析索引へ](../../FIRMWARE_RESEARCH_INDEX.md)

`TryPrepareVoiceRelease`が3212..32f0（遅延中止時3363）を処理する。
AC2Aの要求を消費し、音量・フィルター・ピッチEGを段階12へ移す。
現在値・目標値・レートをコピーし、位相と余りをクリアする。
未発音時の遅延有無、既にリリース済みの分岐、LFOのゼロ状態も保持。

v1.21、CP/DP/EP=0、割り込みマスク7、traceなし、正規voice/indexを要求。
命令数をnative_debtで保持し、周辺回路の12cycle更新を省略しない。
確保・ロック・UI呼び出しを追加せず、既存の音声処理スレッドで動く。

## 検証

- 13,824ケースでH8とのレジスタ/SR/SRAM/PC/命令数一致。
- 5区間のLFO/EGサービスは18,048命令境界で一致。
- Release oracleで379,676ステレオフレームがビット一致。
- Mac Release Shared Codeを署名なしでビルド成功。
- 5区間拡張と合わせ、再生中のfallbackは4,301,519から4,238,291へ減少。
  ピッチ領域fallback=0。これはCPU時間の削減率ではない。

Logic内の負荷、他ROM、全音色、Windows/Linuxは今回未検証。
