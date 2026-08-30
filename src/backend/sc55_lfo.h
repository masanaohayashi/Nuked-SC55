// SC-55 の LFO / 修飾ブロック（ファームウェア 00:3b26-00:3d00）。
//
// ボイスの手前に 34 バイトの修飾ブロックが並んでいて、負の変位で参照される。
// 音量合成が使う 2 系統は、それぞれ別のブロックの +0x06（ランプ出力）と
// +0x20（LFO 出力）に当たる。
//
//   block+0x00  深さ（符号付き）
//   block+0x06  ランプ出力      → 音量合成の修飾 a
//   block+0x0c  レート添字      → rom1[0x7012]
//   block+0x0e  レート修正（符号付き。合計は 0x28f6 で頭打ち）
//   block+0x14  波形の選択      → rom1[0x74c4] のジャンプテーブル
//   block+0x16  位相
//   block+0x18  遅延カウンタ    → 0xffff で飽和
//   block+0x1a  立ち上がりカウンタ
//   block+0x20  LFO 出力        → 音量合成の修飾 depth
//
// 波形は 7 種類（ジャンプテーブルの有効エントリ 0..6）:
//   [0] 正弦   [1] 矩形   [2] 鋸   [3] 台形状   [4][5][6] ランダム（平滑量違い）
#pragma once

#include <cmath>
#include <cstdint>

namespace sc55
{

// LFO の正弦表（rom1[0x7412]、130 バイト）。
// 129 エントリすべてが round(255 * sin(pi * i / 128)) と完全に一致するので式で作る。
inline uint8_t LfoSine (int index)
{
    return (uint8_t) std::lround (255.0 * std::sin (M_PI * index / 128.0));
}

// レートの増分（rom1[0x7012] を引いた値 + 修正）から位相の進みを作る。
// tick は [0xac5a]、ファームウェア全体で使い回されているグローバルな時間刻み。
inline uint16_t LfoIncrement (uint16_t rate, int16_t modifier, uint16_t tick)
{
    uint32_t sum = (uint16_t) (modifier + rate);
    if (sum > 0x28f6) sum = modifier >= 0 ? 0x28f6 : 0;   // 頭打ちの向きが符号で変わる
    return (uint16_t) ((sum * (uint32_t) tick) & 0xffff);
}

// 波形 [0]（00:3bee-00:3c2d）。位相を進めて正弦を引き、符号付き 16 ビットを返す。
// phase は進めた後の値が入って返る。
//
// 実機との照合: TOKMEDLY 19,215/19,215、IMAGA_55 33,611/33,611、GATCHA55 23,438/23,438。
inline int16_t LfoSineStep (uint16_t& phase, uint16_t increment)
{
    phase = (uint16_t) (phase + increment);

    // 0x8000 を折り目にして畳む。
    const uint16_t folded = phase >= 0x8000 ? (uint16_t) (phase - 0x8000)
                                            : (uint16_t) (0x8000 - phase);
    const uint32_t index = (folded >> 8) & 0xff;
    const uint32_t fraction = folded & 0xff;

    const int32_t low = LfoSine ((int) index);
    const int32_t difference = (int32_t) LfoSine ((int) index + 1) - low;

    uint16_t value = difference < 0 ? (uint16_t) ((low << 8) - (-difference) * fraction)
                                    : (uint16_t) ((low << 8) + difference * fraction);
    value = (uint16_t) (value >> 1);

    return phase > 0x8000 ? (int16_t) -(int16_t) value : (int16_t) value;
}

} // namespace sc55
