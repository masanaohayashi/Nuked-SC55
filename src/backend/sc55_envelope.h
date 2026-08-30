// SC-55 のエンベロープ値の符号化（ファームウェア 00:365d-00:36a4）。
//
// PCM チップに渡すワードは「上位バイト = 目標レベル、下位バイト = 変化率」で、
// チップ側が自分でランプする。ファームウェアは毎ティック、前回値との差分を
// 指数 + 仮数のミニ浮動小数に詰めて渡すだけ。
//
// 実機との照合（TVF エンベロープ、20 秒 × 3 曲）:
//   TOKMEDLY 13,459/13,459   IMAGA_55 16,617/16,617   GATCHA55 12,962/12,962
#pragma once

#include "sc55_tables.h"
#include <cstdint>

namespace sc55
{

// 変化量を指数 + 仮数の 1 バイトに詰める。エンベロープもカットオフも同じ符号化で、
// 違うのはシフト上限だけ（エンベロープは 7 固定、カットオフは表引き）。
// 0 が返ったら「保持」を意味する 0xff00 を使う。
inline uint8_t EncodeRate (uint16_t magnitude, int shift_limit)
{
    int exponent = shift_limit;
    bool normalised = false;
    while (true)
    {
        const bool carry = (magnitude & 0x8000) != 0;
        magnitude = (uint16_t) (magnitude << 1);
        if (carry) { normalised = true; break; }
        if (--exponent < 0) break;
    }
    if (! normalised) { exponent = 0; magnitude = (uint16_t) (magnitude >> 1); }

    // 上位バイトから仮数を 4 ビットに丸める（+1 してから 1 ビット落とす）。
    uint32_t mantissa = (magnitude & 0xff00) >> 8;
    mantissa >>= 3;
    mantissa = (mantissa + 1) & 0xff;
    mantissa >>= 1;

    return (uint8_t) (mantissa | ENVELOPE_EXPONENT[exponent]);
}

// カットオフ（00:47b7-00:47fa）。voice+0x26 を作る。
// shift_limit は rom1[0x6b06 + セレクタ] を引いた値。セレクタが 0 なら率は 0xaf 固定。
//
// 実機との照合: TOKMEDLY 13,580/13,580、IMAGA_55 15,197/15,197、GATCHA55 13,470/13,470。
inline uint16_t EncodeCutoff (uint16_t level, uint16_t delta, int shift_limit, bool selector_zero)
{
    if (level > 0xe600) level = 0xe600;
    if (selector_zero) return (uint16_t) ((level & 0xff00) | 0xaf);

    const uint8_t code = EncodeRate (delta, shift_limit);
    return code == 0 ? 0xff00 : (uint16_t) ((level & 0xff00) | code);
}

// prev / next はエンベロープの前回値と今回値（voice+0x1c）。返り値は voice+0x1e。
inline uint16_t EncodeEnvelope (uint16_t prev, uint16_t next)
{
    const int32_t delta = (int32_t) next - (int32_t) prev;
    if (delta == 0)
        return 0xff00;                       // 動いていない = 保持

    uint16_t level = next;
    uint16_t magnitude;

    if (delta < 0)
    {
        magnitude = (uint16_t) -delta;
    }
    else
    {
        magnitude = (uint16_t) delta;
        // 上がっていて上位バイトが変わらないなら、目標を 1 段先に置いて進みを保証する。
        if ((prev & 0xff00) == (level & 0xff00))
        {
            const uint32_t bumped = (uint32_t) level + 0x100;
            level = bumped > 0xffff ? 0xff00 : (uint16_t) bumped;
        }
    }

    const uint8_t code = EncodeRate (magnitude, 7);
    return code == 0 ? 0xff00 : (uint16_t) ((level & 0xff00) | code);
}

} // namespace sc55
