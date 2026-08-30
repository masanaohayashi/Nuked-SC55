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

// 波形 [4]（00:3cac-00:3ccf）。素のサンプル&ホールド。
//
// 32 ビットに広げた位相を進め、上位に桁上がりが出たときだけ新しい値を抽選して保持する。
// 出力は保持値そのもので、平滑も補間もしない。
//
// **抽選元は追い切れていない。** `mov:g.b #0x1e,@0x3e` で何かに合図してから
// `@0x3a`（ベースレジスタ経由で 0xe03a）を読む、という形になっていて、そこは
// 一時変数。外から覗くと毎回違う値に見えるが、実際に読まれる瞬間は 3 曲とも
// 一貫して 0xffff だった。合図の相手を特定するまで、ここは「観測された定数」として扱う。
//
// 実機との照合（波形 [0] と [4] を合わせて）:
//   TOKMEDLY 27,195/27,195   IMAGA_55 34,173/34,173   GATCHA55 31,181/31,181
inline uint16_t LfoSampleHold (uint32_t& phase32, uint32_t increment32,
                               uint16_t held, uint16_t drawn)
{
    const uint32_t doubled = increment32 << 1;
    const uint32_t low  = (doubled & 0xffff) + (phase32 & 0xffff);
    const uint32_t high = ((doubled >> 16) & 0xffff) + (low >> 16);

    phase32 = low & 0xffff;
    return (high & 0xffff) != 0 ? drawn : held;
}

// 波形 [1] 矩形（00:3c31）と [2] 鋸（00:3c48）。
// **この 2 つは 3 曲とも一度も使われないので、逆アセンブルを読んだだけで未検証。**
inline int16_t LfoSquare (uint16_t phase) { return phase < 0x8000 ? 0x7fff : (int16_t) 0x8001; }
inline int16_t LfoSaw    (uint16_t phase) { return (int16_t) (uint16_t) (phase - 0x8000); }

// 波形 [3] 台形（00:3c5c-00:3cab）。0x8000 と 0x4000 で二段に折って作る三角。
//
// 後半（位相 >= 0x8000）は `r3` を計算して符号反転した直後に **`r4` を格納**していて、
// 素直に読むと辻褄が合わない。読んだままに実装したところ 440 件すべて一致したので、
// これはファームウェアがそう書かれているということ。直さない。
//
// 実機との照合: IMAGA_55 440/440（他の 2 曲では一度も通らない）。
inline int16_t LfoTrapezoid (uint16_t phase)
{
    uint16_t folded = (uint16_t) (phase - 0x8000);
    if (folded == 0) return 0;

    const bool second_half = phase >= 0x8000;
    if (! second_half) folded = (uint16_t) (-(int16_t) folded);

    const uint16_t before = folded;
    folded = (uint16_t) (folded - 0x4000);

    uint16_t doubled = 0, ramp;
    if (folded == 0)
    {
        ramp = 0x7fff;
    }
    else
    {
        if (before < 0x4000) folded = (uint16_t) (-(int16_t) folded);
        doubled = (uint16_t) (folded * 2);
        ramp = (uint16_t) (0x8000 - doubled);
    }

    return (int16_t) (second_half ? doubled : ramp);
}

} // namespace sc55
