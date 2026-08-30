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

    // 正規化。最上位ビットが立つまで左シフトし、シフト回数を指数に使う。
    int exponent = 7;
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

    const uint8_t code = (uint8_t) (mantissa | ENVELOPE_EXPONENT[exponent]);
    return code == 0 ? 0xff00 : (uint16_t) ((level & 0xff00) | code);
}

} // namespace sc55
