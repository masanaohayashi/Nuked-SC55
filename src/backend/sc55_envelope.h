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

// エンベロープのセグメント（00:35c9-00:365b）。開始レベルと終了レベルのあいだを
// 進捗 0..0xffff で補間する。モードは 2 つ:
//
//   線形     voice-8 == 0。差分に進捗を掛けて足すだけ
//   指数接近 それ以外。ENVELOPE_CURVE を隣接 2 点で補間し、幅に掛ける
//
// 上がりと下がりで別経路になっていて、上がりは曲線を反転して開始から積み上げ、
// 下がりは曲線をそのまま終端へ足し込む。レベルはバイト値を上位バイトに置いたもの。
//
// 実機との照合（この関数 + EncodeEnvelope の通し、全枝を含む）:
//   TOKMEDLY 13,459/13,459   IMAGA_55 16,617/16,617   GATCHA55 12,962/12,962
inline uint16_t EnvelopeSegment (uint8_t start_level, uint8_t end_level,
                                 uint16_t progress, bool exponential)
{
    const uint16_t start = (uint16_t) (start_level << 8);
    const uint16_t end   = (uint16_t) (end_level << 8);

    if (progress == 0xffff)
        return end;                                   // 到達済み

    if (! exponential)
    {
        if (end >= start) return (uint16_t) ((((uint32_t) (end - start) * progress) >> 16) + start);
        return (uint16_t) (start - (((uint32_t) (start - end) * progress) >> 16));
    }

    // 進捗を反転して表を引き、下位バイトで隣接 2 点を補間する。
    const uint16_t inverted = (uint16_t) ~progress;
    const uint32_t fraction = inverted & 0xff;
    const uint32_t index    = (inverted & 0xff00) >> 8;

    const uint16_t low  = ENVELOPE_CURVE[index];
    const uint16_t step = (uint16_t) (ENVELOPE_CURVE[index + 1] - low);
    const uint16_t curve = (uint16_t) ((((fraction * step) >> 8) & 0xffff) + low);

    if (end >= start)
        return (uint16_t) ((((uint32_t) (uint16_t) (end - start) * (uint16_t) ~curve) >> 16) + start);

    return (uint16_t) ((((uint32_t) curve * (uint16_t) (start - end)) >> 16) + end);
}

// カットオフの通し（00:473c-00:47eb）。voice+0x26 を作る。
//
//   control  voice+0x22（曲線を引く 16 ビット値）
//   previous voice+0x24（前回のカットオフ）
//   flag     voice+0x68（天井表の添字。8 未満は 8 に切り上げる）
//   limit    rom1[0x6b06 + セレクタ]。セレクタが 0 なら率は 0xaf 固定
//
// level_now には今回のカットオフが返るので voice+0x24 に書き戻す。
//
// 実機との照合（曲線引きから符号化まで通し）:
//   TOKMEDLY 13,720/13,720   IMAGA_55 16,325/16,325   GATCHA55 13,835/13,835
inline uint16_t CutoffWord (uint16_t control, uint16_t previous, uint8_t flag,
                            int limit, bool selector_zero, uint16_t& level_now)
{
    const uint32_t index = (control >> 8) & 0xff;
    uint32_t level = CUTOFF_CURVE[index];
    if ((control & 0xff) != 0)                      // 下位バイトで隣と補間
    {
        const uint16_t step = (uint16_t) (CUTOFF_CURVE[index + 1] - (uint16_t) level);
        level = (uint16_t) (level + (((((control & 0xff) * step) >> 8)) & 0xffff));
    }
    level = (level * 2) & 0xffff;

    if (flag < 8) flag = 8;
    const uint32_t ceiling = (uint32_t) (CUTOFF_CEILING[flag] << 8);
    if (ceiling < level) level = ceiling;
    if (level > 0xe600) level = 0xe600;

    // voice+0x24 への書き戻しは 00:478b、つまり下の低位バイト落としより前に起きる。
    level_now = (uint16_t) level;

    const int32_t delta = (int32_t) level - (int32_t) previous;
    if (delta == 0) return 0xff00;

    uint16_t magnitude;
    if (delta < 0)
    {
        magnitude = (uint16_t) -delta;
    }
    else
    {
        magnitude = (uint16_t) delta;
        // 上がっていて上位バイトが変わらないなら 1 段先へ。天井は改めて当てる。
        const uint32_t high = level & 0xff00;
        if (high == (uint32_t) (previous & 0xff00))
        {
            level = high + 0x100;
            if (ceiling < level) level = ceiling;
        }
        else level = high;
    }
    if (level > 0xe600) level = 0xe600;

    if (selector_zero)
        return (uint16_t) ((level & 0xff00) | 0xaf);

    const uint8_t code = EncodeRate (magnitude, limit);
    return code == 0 ? 0xff00 : (uint16_t) ((level & 0xff00) | code);
}

} // namespace sc55
