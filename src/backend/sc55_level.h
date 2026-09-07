// SC-55 の音量合成（ファームウェア 00:309b-00:3126）をネイティブに書き直したもの。
//
// パート音量・エクスプレッション・マスターボリューム・音色ごとの係数を掛け合わせ、
// 2 系統の修飾（LFO など）を足し、最後に二乗してカーブを付ける。
//
// 実機との照合（20 秒 × 3 曲、呼び出しごとに返り値を突き合わせ）:
//   TOKMEDLY 14,150/14,150   IMAGA_55 17,365/17,365   GATCHA55 15,008/15,008
//
// 詳しい導出と、途中で外した 2 箇所は FIRMWARE_STRUCTURE.md を参照。
#pragma once

#include <cstdint>
#include <array>
#include <algorithm>

namespace sc55
{

struct LevelInputs
{
    // 表引き 2 段の正体（あとから CC の差分で判明）:
    //   [0xc8e4 + reg1]      そのボイスのパート番号（0xa318[slot] と 3 曲 100% 一致）
    //   [0xab36 + パート]    そのパートのエクスプレッション（CC11 の置き場所）
    uint8_t expression   = 0;   // = [0xab36 + パート]
    uint8_t velocity     = 0;   // legacy name: part +0x08 = CC7 volume, NOT note-on velocity
    uint8_t master       = 0;   // [0x8002]
    uint8_t tone_scale   = 0;   // 音色ごとの係数（+0x30 が指す先の +0x100）。0 なら未使用
    bool    has_tone_scale = false;

    int16_t bias         = 0;   // +0x8a
    int16_t mod1_a = 0, mod1_b = 0, mod1_depth = 0;   // -122, +0x8e, -96
    int16_t mod2_a = 0, mod2_b = 0, mod2_depth = 0;   // -88,  +0x96, -62
};

// 00:312b。同符号の深さの和だけを ±0x7f00 に制限し、積の端数は切り上げる。
// r4 は16ビット。加算は折り返し、減算は各モジュレーションごとにゼロで止める。
inline void ApplyModulation (int32_t& level, int16_t a, int16_t b, int16_t depth)
{
    int32_t amount;
    bool negative;

    if (a < 0 && b < 0)      { amount = uint16_t(-(int32_t) a + -(int32_t) b); negative = true; }
    else if (a < 0 || b < 0) { amount = (int32_t) a + b; negative = amount < 0; if (negative) amount = -amount; }
    else                     { amount = (int32_t) a + b; negative = false; }

    // The cap is present only on same-sign paths. Mixed signs bypass it;
    // the double -32768 case wraps the magnitude sum to zero before capping.
    if ((a < 0) == (b < 0) && amount > 0x7f00) amount = 0x7f00;

    int32_t magnitude = depth;
    const bool flip = magnitude < 0;
    if (flip) magnitude = -magnitude;

    const uint32_t shifted = ((uint32_t) amount * (uint32_t) magnitude) << 1;
    const uint32_t high = (shifted >> 16) + ((shifted & 0xffff) != 0 ? 1u : 0u);

    // H8 r4 is a word. SUBX clamps a borrow to zero (3181..3185),
    // while ADDX wraps. Clamp each source, not just the combined result.
    const auto current = uint16_t(level);
    if (negative != flip) level = current < high ? 0 : int32_t(current-high);
    else                  level = uint16_t(current+high);
}

inline uint16_t ComputeLevel (const LevelInputs& in)
{
    // 三段の掛け算。バイトの詰め替え（mov:g.b / swap.b）は 32 ビット積からの >>8 抽出。
    const uint32_t product = ((uint32_t) in.expression * in.velocity * in.master) << 2;
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

// Native owner of voice+06/+18/+1a. PCM readback updates level before
// advance; output writes consume command afterwards. No ticks multiplier:
// the firmware increments the ramp once per invocation of 36db..3734.
struct TvaState
{
    uint16_t ramp = 0, level = 0, command = 0xff00;

    // Normal new-voice path (-3b bit7): 2c5d sets the ramp, then
    // 3080..308d installs the unattenuated level and initial rate ba.
    // Not the voice-reuse fade path, and never called on periodic updates.
    void initialize(const LevelInputs& inputs) noexcept
    {
        ramp = 65535;
        level = ComputeLevel(inputs);
        command = uint16_t((level&0xff00)|0xba);
    }

    // false corresponds to the 3363 stop branch. The outer voice task must
    // stop dispatching, rather than treating this as an ordinary held command.
    bool advance(uint16_t firstStage,const LevelInputs& inputs) noexcept
    {
        if (firstStage > 12) return false;
        const auto target = ComputeLevel(inputs);
        const uint32_t nextRamp = uint32_t(ramp)+0x2000;
        ramp = nextRamp > 65535 ? 65535 : uint16_t(nextRamp);
        command = TvaWord(target,ramp,level,level);
        return true;
    }
};

// エフェクト送り voice+0x3a（00:37b1-00:37f8）。
//
// 下位バイトがコーラス(CC93)、上位バイトがリバーブ(CC91)。目標値（+0x0e / +0x0f）へ
// 引数名 reverb_target / chorus_target は旧名が逆のままなので、下位 / 上位として扱う。
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

using PanTable = std::array<uint8_t,129>; // data at v1.21 6c8f..6d0f

struct SpatialInputs
{
    uint8_t pan = 64, basePan = 64, masterPan = 0;
    uint8_t reverb = 0, chorus = 0; // legacy reversed names: low CC93 / high CC91
    bool hasToneScale = false;
    uint8_t panScale = 64, reverbScale = 0, chorusScale = 0; // low/high send scales, respectively
};

struct SpatialState
{
    uint16_t pan = 64, panWord = 0, effects = 0; // voice +36/+34/+3a

    // 304c/3067..307d: install the already resolved pan position, including
    // random-pan's frozen sentinel. Unlike periodic motion this MUST encode
    // the gains even when the starting position equals the next target.
    // Position selection/random sampling and initial sends belong to caller.
    bool initializePan(uint8_t position,bool frozen,const PanTable& table) noexcept
    {
        if (position > 128) return false;
        pan = frozen ? 65535 : position;
        panWord = uint16_t((uint16_t(table[128-position])<<8)|table[position]);
        return true;
    }

    // 2fcc..3080. Initial sends are installed directly, not smoothed from
    // zero. Part pan0 or an enabled tone pan-scale0 selects random/frozen pan.
    // All controls are validated before the optional PCM random transaction.
    template<class Read,class Write>
    bool initialize(const SpatialInputs& input,const PanTable& table,Read&& read,Write&& write)
    {
        if (input.pan > 127 || input.basePan > 127 || input.masterPan > 127
            || (input.hasToneScale && input.panScale > 127)) return false;
        const auto send = [&](uint8_t value,uint8_t scale) {
            return input.hasToneScale ? uint8_t((unsigned(value)*scale*2+255)>>8) : value;
        };
        const bool random = input.pan == 0 || (input.hasToneScale && input.panScale == 0);
        uint8_t position;
        if (random)
        {
            write(uint8_t(0x3e),uint8_t(30)); (void)read(uint8_t(0x34));
            const auto high = read(uint8_t(0x3a)); (void)read(uint8_t(0x3b));
            position = uint8_t(high>>1);
        }
        else
        {
            const auto adjust = [](int value,int control) { return std::clamp(value+control-64,0,127); };
            int target = input.hasToneScale ? adjust(input.pan,input.panScale) : input.pan;
            target = adjust(input.basePan,target);
            if (input.masterPan != 0) target = adjust(target,input.masterPan);
            position = uint8_t(target);
        }
        initializePan(position,random,table);
        effects = uint16_t(send(input.reverb,input.reverbScale)|(uint16_t(send(input.chorus,input.chorusScale))<<8));
        return true;
    }

    // 3734..37fb. Pan ffff freezes both position and encoded word, but
    // effects still move. Neither pan nor sends multiply by elapsed ticks.
    bool advance(const SpatialInputs& input,const PanTable& table) noexcept
    {
        if (pan != 65535)
        {
            if (pan > 128 || input.pan > 127 || input.basePan > 127 || input.masterPan > 127
                || (input.hasToneScale && input.panScale > 127)) return false;
            const auto adjust = [](int value,int control) { return std::clamp(value+control-64,0,127); };
            int target = input.hasToneScale ? adjust(input.pan,input.panScale) : input.pan;
            target = adjust(input.basePan,target);
            if (input.masterPan != 0) target = adjust(target,input.masterPan);
            if (pan != target)
            {
                pan = uint16_t(pan < target ? pan+1 : pan-1);
                panWord = uint16_t((uint16_t(table[128-pan])<<8)|table[pan]);
            }
        }
        effects = EffectSend(effects,input.reverb,input.chorus,
            input.hasToneScale ? &input.reverbScale : nullptr,
            input.hasToneScale ? &input.chorusScale : nullptr);
        return true;
    }
};

struct VoiceOutputState
{
    TvaState tva;
    SpatialState spatial;
    enum class Result { invalidInput, stopped, updated };
    // Full output-control calculation at 36db..37fb; no device writes here.
    Result advance(uint16_t firstStage,const LevelInputs& level,const SpatialInputs& input,const PanTable& table) noexcept
    {
        if (firstStage > 12) return Result::stopped;
        auto next = *this;
        next.tva.advance(firstStage,level);
        if (!next.spatial.advance(input,table)) return Result::invalidInput;
        *this = next;
        return Result::updated;
    }
};

// 音量合成が修飾の b 側に読む 3 つのコントローラ値（00:5cee / 00:5e59 / 00:5eb0）。
//
// 3 箇所とも同じ形をしている: 頭打ち → 16 倍 → 係数を掛けて上位ワードを取る。
// 頭打ちと係数だけが違う。
//
//   voice+0x8a   cap 0x0fa0   係数 0x820d
//   voice+0x8e   cap 0x0fc0   係数 0x8105
//   voice+0x96   cap 0x0fc0   係数 0x8105
//
// バレルシフタが無いので 16 倍は `add r4,r4` を 4 回並べてある。
//
// 実機との照合（3 フィールドとも）:
//   TOKMEDLY 12,621/12,621   IMAGA_55 17,001/17,001   GATCHA55 15,733/15,733
inline uint16_t ScaleController (uint16_t value, uint16_t cap, uint32_t coefficient)
{
    if (value >= cap) value = cap;
    return (uint16_t) ((((uint32_t) (uint16_t) (value << 4)) * coefficient) >> 16);
}

} // namespace sc55
