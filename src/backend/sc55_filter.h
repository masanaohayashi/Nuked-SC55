// SC-55 のフィルタ設定（ファームウェア 00:3e30-00:3e6c、00:41e4-00:4250）。
//
// PCM チップに渡る voice+0x66（モード）、+0x68（レゾナンス）、+0x69（その上限）を作る。
// どれもノートオン時に一度だけ決まる。
//
// 実機との照合:
//   +0x66  857/857、485/485、720/720
//   +0x67  682/682、359/359、596/596
//   +0x68  682/682、359/359、596/596
//   +0x69  682/682、359/359、596/596
#pragma once

#include "sc55_tables.h"
#include <cstdint>

namespace sc55
{

struct FilterSetup
{
    uint8_t mode = 0;        // voice+0x66
    uint8_t cutoff_base = 0; // voice+0x67
    uint8_t resonance = 0;   // voice+0x68
    uint8_t limit = 0;       // voice+0x69
    bool    bypass = false;  // モード 2 以上はフィルタを通さない
};

// partial_mode は パーシャル +0x27、partial_cutoff は +0x26。
inline FilterSetup FilterMode (uint8_t partial_mode, uint8_t partial_cutoff)
{
    FilterSetup out;
    if (partial_mode < 2)
    {
        out.mode = (uint8_t) ((partial_mode * 2) | 1);   // 0 → 1、1 → 3
        out.cutoff_base = partial_cutoff;
    }
    else
    {
        out.mode = 3;                                     // バイパス
        out.resonance = 0x40;
        out.bypass = true;
    }
    return out;
}

// カットオフ曲線から、そのボイスで許されるレゾナンスの上限を引く（00:4202-00:4221）。
inline uint8_t ResonanceLimit (int32_t cutoff_value)
{
    int32_t v = (int32_t) (int16_t) (cutoff_value + 0xff);
    if (v < 0) v = 0x7fff;

    uint32_t scaled = (uint32_t) CUTOFF_CURVE[((uint16_t) v >> 8) & 0xff] * 2 + 0xff;
    if (scaled > 0xffff) scaled = 0xff00;

    return RESONANCE_LIMIT[(scaled >> 8) & 0xff];
}

// レゾナンス voice+0x68（00:4224-00:4250）。partial_resonance は パーシャル +0x13
// （0x40 が中央）。中央からのずれを 2 倍して基準に足し引きし、上限で抑える。
inline uint8_t Resonance (uint8_t cutoff_base, uint8_t partial_resonance, uint8_t limit)
{
    const int8_t offset = (int8_t) (partial_resonance - 0x40);

    if (offset >= 0)
    {
        const int8_t after = (int8_t) (cutoff_base - (int8_t) (offset * 2));
        if (after < 0) return 0;
        return (uint8_t) after < limit ? (uint8_t) after : limit;
    }

    const uint8_t after = (uint8_t) (cutoff_base + (int8_t) ((int8_t) -offset * 2));
    return after < limit ? after : limit;
}

} // namespace sc55
