// SC-55 の音量合成（ファームウェア 00:309b-00:3126）をネイティブに書き直したもの。
//
// パート音量・ベロシティ・マスターボリューム・音色ごとの係数を掛け合わせ、
// 2 系統の修飾（LFO など）を足し、最後に二乗してカーブを付ける。
//
// 実機との照合（20 秒 × 3 曲、呼び出しごとに返り値を突き合わせ）:
//   TOKMEDLY 14,150/14,150   IMAGA_55 17,365/17,365   GATCHA55 15,008/15,008
//
// 詳しい導出と、途中で外した 2 箇所は FIRMWARE_STRUCTURE.md を参照。
#pragma once

#include <cstdint>

namespace sc55
{

struct LevelInputs
{
    uint8_t part_volume  = 0;   // [0xab36 + [0xc8e4 + パート]] を引いた後の値
    uint8_t velocity     = 0;   // 音色ポインタ +0x08
    uint8_t master       = 0;   // [0x8002]
    uint8_t tone_scale   = 0;   // 音色ごとの係数（+0x30 が指す先の +0x100）。0 なら未使用
    bool    has_tone_scale = false;

    int16_t bias         = 0;   // +0x8a
    int16_t mod1_a = 0, mod1_b = 0, mod1_depth = 0;   // -122, +0x8e, -96
    int16_t mod2_a = 0, mod2_b = 0, mod2_depth = 0;   // -88,  +0x96, -62
};

// 00:312b。r4 += (a + b) * |depth| >> 15。和は ±0x7f00 で飽和し、端数は切り上げる。
inline void ApplyModulation (int32_t& level, int16_t a, int16_t b, int16_t depth)
{
    int32_t amount;
    bool negative;

    if (a < 0 && b < 0)      { amount = -(int32_t) a + -(int32_t) b; negative = true; }
    else if (a < 0 || b < 0) { amount = (int32_t) a + b; negative = amount < 0; if (negative) amount = -amount; }
    else                     { amount = (int32_t) a + b; negative = false; }

    if (amount > 0x7f00) amount = 0x7f00;

    int32_t magnitude = depth;
    const bool flip = magnitude < 0;
    if (flip) magnitude = -magnitude;

    const uint32_t shifted = ((uint32_t) amount * (uint32_t) magnitude) << 1;
    const uint32_t high = (shifted >> 16) + ((shifted & 0xffff) != 0 ? 1u : 0u);

    if (negative != flip) level -= (int32_t) high;
    else                  level += (int32_t) high;
}

inline uint16_t ComputeLevel (const LevelInputs& in)
{
    // 三段の掛け算。バイトの詰め替え（mov:g.b / swap.b）は 32 ビット積からの >>8 抽出。
    const uint32_t product = ((uint32_t) in.part_volume * in.velocity * in.master) << 2;
    uint32_t scaled = (product >> 8) & 0xffff;

    if (in.has_tone_scale)
    {
        scaled = (((scaled * in.tone_scale) << 1) >> 8) & 0xffff;
        scaled = (uint32_t) (((uint64_t) scaled * 0x830e) >> 15) & 0xffff;
    }
    else
    {
        scaled = (uint32_t) (((uint64_t) scaled * 0x8208) >> 15) & 0xffff;
    }

    int32_t level = (int32_t) scaled;
    if (level == 0)
        return 0;

    if (in.bias != 0)
    {
        level += in.bias;
        if (level < 0) level = 0;         // 負側だけ 0 で止める
    }

    ApplyModulation (level, in.mod1_a, in.mod1_b, in.mod1_depth);
    ApplyModulation (level, in.mod2_a, in.mod2_b, in.mod2_depth);

    // 二乗してカーブを付け、係数を掛けてから上位 16 ビットを取り出す。
    const uint32_t squared = ((uint32_t) (level & 0xffff) * (uint32_t) (level & 0xffff)) >> 16;
    const uint32_t final32 = squared * 0x208;

    return (final32 >> 16) >= 0xff ? 0xffff : (uint16_t) ((final32 >> 8) & 0xffff);
}

// TVA が PCM チップへ渡すワード（00:36db-00:372f）。
//
// ComputeLevel の結果に立ち上がりのランプを掛けたものが今回のレベル。前回との差が
// 小さければ「保持」、動いていれば率は固定値 0xb4 を使う。エンベロープやカットオフと
// 違って、TVA は率を可変で送らない。
//
// ramp は voice+0x06（0xffff でなければ毎ティック 0x2000 ずつ飽和加算）、
// previous は voice+0x18。level_now には今回のレベルが返るので voice+0x18 に書き戻す。
//
// 実機との照合（音量合成と符号化を繋いだ通し）:
//   TOKMEDLY 13,293/13,293   IMAGA_55 16,880/16,880   GATCHA55 14,288/14,288
inline uint16_t TvaWord (uint16_t level, uint16_t ramp, uint16_t previous, uint16_t& level_now)
{
    const uint32_t now = ((uint32_t) level * ramp) >> 16;
    level_now = (uint16_t) now;

    const int32_t delta = (int32_t) now - (int32_t) previous;
    const uint32_t magnitude = delta < 0 ? (uint32_t) -delta : (uint32_t) delta;

    if (magnitude <= 0x10)
        return 0xff00;                                  // 保持

    return (uint16_t) ((now & 0xff00) | 0xb4);          // 率は固定
}

// エフェクト送り voice+0x3a（00:37b1-00:37f8）。
//
// 下位バイトがリバーブ、上位バイトがコーラス。音色の目標値（+0x0e / +0x0f）へ
// **毎ティック 1 ずつしか動かない**スルーレート制限になっている。急な送り量の変化で
// エフェクトが跳ねないようにするためで、値そのものより変化の速さを縛る作り。
//
// セレクタ（voice+0x30）が 0 でなければ、目標値は表で目減りさせてから使う。
//
// 実機との照合:
//   TOKMEDLY 13,293/13,293   IMAGA_55 16,880/16,880   GATCHA55 14,288/14,288
inline uint16_t EffectSend (uint16_t current, uint8_t reverb_target, uint8_t chorus_target,
                            const uint8_t* reverb_scale = nullptr,
                            const uint8_t* chorus_scale = nullptr)
{
    uint32_t low = reverb_target, high = chorus_target;

    if (reverb_scale != nullptr)   // セレクタありのときは表で縮める（切り上げ）
    {
        low  = ((low  * *reverb_scale * 2 + 0xff) >> 8) & 0xff;
        high = ((high * *chorus_scale * 2 + 0xff) >> 8) & 0xff;
    }

    int lo = current & 0xff, hi = (current >> 8) & 0xff;
    if (lo != (int) low)  lo += ((int) low  >= lo) ? 1 : -1;
    if (hi != (int) high) hi += ((int) high >= hi) ? 1 : -1;

    return (uint16_t) (((hi & 0xff) << 8) | (lo & 0xff));
}

} // namespace sc55
