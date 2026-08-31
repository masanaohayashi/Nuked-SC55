// ファームウェアの一部を、同じ結果を出すネイティブなコードに置き換える。
//
// エミュレータの費用はほぼ全部が命令解釈で、実測では音声コールバックの 85%。
// 1 命令あたりを速くする余地はもう無い（オペランド解決を表引きにしても変わらなかった）。
// 減らせるのは実行する命令の数だけで、そのためには何をしているかを理解して、
// 同じ結果を出す C++ に置き換えるしかない。
//
// 置き換えが成立する条件は 2 つある。
//
//   1. メモリに残る結果が一致すること
//   2. 消費するサイクル数が一致すること
//
// 2 を外すと割り込みの位置がずれて音が変わる。命令を飛ばすだけの実装では実際に
// 3.19 dB 変わった。だから各置き換えは、実行したはずの命令数も一緒に計算する。
// この機種は 1 命令 12 サイクル固定なので、命令数が分かればサイクル数も分かる。
//
// 置き換えるのは信号処理と MIDI の側だけ。表示のためにファームウェアが回している
// ぶん（レベルメーターなど）は、速くするのではなく processBlock の外へ出すもの。
#pragma once

#include "mcu.h"
#include "sc55_level.h"
#include <cstdint>
#include <cstdlib>

namespace mcu_native
{

inline uint16_t ReadWord (mcu_t& mcu, uint32_t at)
{
    return (uint16_t) ((MCU_Read (mcu, at) << 8) | MCU_Read (mcu, at + 1));
}

// 00:312b-0x3187 の修飾。level += (a + b) * |depth| >> 15。
// 経路ごとの命令数を数えながら sc55::ApplyModulation と同じ計算をする。
// 深さの絶対値は r6 に残ったまま呼び出し元へ戻る。
inline uint16_t g_modulate_low = 0;

inline uint32_t Modulate (int32_t& level, int16_t a, int16_t b, int16_t depth)
{
    uint32_t n = 0;

    int32_t amount;
    bool negative;

    if (a < 0)
    {
        n += 2;                        // 312b, 312d
        if (b < 0)
        {
            n += 6;                    // 313f..3151 の枝
            amount = -(int32_t) a + -(int32_t) b;
            negative = true;
        }
        else
        {
            n += 4;                    // 3153..3159
            amount = (int32_t) a + b;
            negative = amount < 0;
            if (negative) { amount = -amount; }
        }
    }
    else
    {
        n += 4;                        // 312b, 312d, 312f, 3131
        if (b < 0)
        {
            n += 4;
            amount = (int32_t) a + b;
            negative = amount < 0;
            if (negative) amount = -amount;
        }
        else
        {
            n += 3;                    // 3133, 3135, 3138
            amount = (int32_t) a + b;
            negative = false;
        }
    }

    if (amount > 0x7f00) { amount = 0x7f00; n += 2; }

    n += 2;                            // 315b, 315d
    int32_t magnitude = depth;
    const bool flip = magnitude < 0;
    if (flip)
    {
        magnitude = -magnitude;
        n += 2;                        // 315f, 3161
    }
    n += 6;                            // 3169.. か 3177.. のどちらも 6 命令

    g_modulate_low = (uint16_t) magnitude;

    const uint32_t shifted = ((uint32_t) amount * (uint32_t) magnitude) << 1;
    const uint32_t high = (shifted >> 16) + ((shifted & 0xffff) != 0 ? 1u : 0u);

    if (negative != flip)
    {
        // 0x3181 の subx で借りが出たら 0x3185 が結果を 0 にする。
        const int32_t next = level - (int32_t) high;
        if (next < 0) { level = 0; n += 1; }
        else            level = next;
    }
    else
    {
        level += (int32_t) high;
    }

    return n;
}

// 呼び出し元に見える出口の状態。r0/r1/r7 と sr 上位は実測で保存されるので持たない。
struct LevelExit
{
    uint16_t r2, r3, r4, r5, r6;
    uint8_t  flags;                // sr 下位（N/Z/V/C）
    uint16_t pc;                   // 戻った rts の番地。経路の識別用
};

// 00:309b-0x3126 の音量合成。r5 に結果を残して戻る。
// 返り値は実行したはずの命令数。
inline uint32_t ComputeLevel (mcu_t& mcu, uint16_t voice, uint16_t reg1, uint16_t& result, LevelExit* out = nullptr)
{
    uint32_t n = 16;                   // 309b..30bf

    const uint32_t page = (uint32_t) mcu.dp << 16;
    const uint32_t expression = MCU_Read (mcu, page | ((MCU_Read (mcu, 0xc8e4u + reg1) + 0xab36u) & 0xffff));
    const uint32_t patch = ReadWord (mcu, voice + 0x2e);
    const uint32_t velocity = MCU_Read (mcu, page | ((patch + 8) & 0xffff));
    const uint32_t master = MCU_Read (mcu, page | 0x8002u);

    uint32_t scaled = (((expression * velocity * master) << 2) >> 8) & 0xffff;

    n += 2;                            // 30c1, 30c4
    const uint16_t selector = ReadWord (mcu, voice + 0x30);
    if (selector != 0)
    {
        n += 9;                        // 30c6..30da
        const uint32_t d = MCU_Read (mcu, page | ((selector + 0x100) & 0xffff));
        scaled = (((scaled * d) << 1) >> 8) & 0xffff;
        scaled = (uint32_t) (((uint64_t) scaled * 0x830e) >> 15) & 0xffff;
    }
    else
    {
        n += 1;                        // 30dc
        scaled = (uint32_t) (((uint64_t) scaled * 0x8208) >> 15) & 0xffff;
    }

    n += 4;                            // 30e0, 30e2, 30e4, 30e6
    int32_t level = (int32_t) (scaled & 0xffff);
    if (level == 0)
    {
        n += 2;                        // 30e8, 30ea
        result = 0;
        if (out) *out = LevelExit { 0, 0, 0, 0, 0, 0, 0x30ea };
        return n;
    }

    n += 2;                            // 30eb, 30ef
    const int16_t bias = (int16_t) ReadWord (mcu, voice + 0x8a);
    if (bias != 0)
    {
        n += 3;                        // 30f1..30fd のどちらか
        level += bias;
        if (level < 0) { level = 0; n += 2; }
    }

    n += 4;                            // 30ff, 3102, 3106, 3109（bsr）
    n += Modulate (level,
                   (int16_t) ReadWord (mcu, voice - 122),
                   (int16_t) ReadWord (mcu, voice + 0x8e),
                   (int16_t) ReadWord (mcu, voice - 96));
    n += 1;                            // rts

    n += 4;                            // 310b, 310e, 3112, 3115（bsr）
    n += Modulate (level,
                   (int16_t) ReadWord (mcu, voice - 88),
                   (int16_t) ReadWord (mcu, voice + 0x96),
                   (int16_t) ReadWord (mcu, voice - 62));
    n += 1;                            // rts

    n += 4;                            // 3117, 3119, 311d, 3120
    const uint32_t squared = ((uint32_t) (level & 0xffff) * (uint32_t) (level & 0xffff)) >> 16;
    const uint32_t final32 = squared * 0x208;

    if ((final32 >> 16) >= 0xff)
    {
        n += 2;                        // 3127, 312a
        result = 0xffff;
        if (out) *out = LevelExit { 0, 0, (uint16_t) (final32 >> 16), result, g_modulate_low, 0, 0x312a };
    }
    else
    {
        n += 2;                        // 3122, 3124（3126 の rts は 3120 側で数えた）
        result = (uint16_t) ((final32 >> 8) & 0xffff);
        if (out)
            *out = LevelExit { 0, 0xffff, (uint16_t) (final32 >> 16), result, g_modulate_low,
                               (uint8_t) (0x01u | (result & 0x8000u ? 0x08u : 0u) | (result == 0 ? 0x04u : 0u)),
                               0x3126 };
    }

    return n;
}

// 00:309b にいるときに呼ぶ。置き換えられたら true。
// 出口レジスタは実測で確かめてある: r0/r1/r7 と sr 上位は保存、r2..r6 と sr 下位は
// ここで作る値になる。経路 0x3126 以外は未検証なので、その場合は置き換えない。
// 0x309b の中身が想定どおりかを ROM で確かめる。他のファームウェア（v1.00, SCC-1A,
// mk1, JV880）では同じ番地に別の処理があるので、一致しなければ置き換えない。
inline bool RomMatches (const mcu_t& mcu)
{
    static const uint8_t sig[] = { 0xaa, 0x13, 0xf1, 0xc8, 0xe4, 0x82, 0xf2, 0xab,
                                   0x36, 0x82, 0xe8, 0x2e, 0x83, 0xe3, 0x08, 0xaa };
    for (size_t i = 0; i < sizeof sig; ++i)
        if (mcu.rom1[(0x309b + i) & 0x7fff] != sig[i])
            return false;
    return true;
}

inline bool TryComputeLevel (mcu_t& mcu)
{
    if (mcu.native_ok < 0)
        mcu.native_ok = (RomMatches (mcu) && std::getenv ("SC55_NONATIVE") == nullptr) ? 1 : 0;
    if (mcu.native_ok == 0)
        return false;

    uint16_t result = 0;
    LevelExit out {};
    const uint32_t n = ComputeLevel (mcu, (uint16_t) mcu.r[0], (uint16_t) mcu.r[1], result, &out);

    if (out.pc != 0x3126)
        return false;

    mcu.r[2] = out.r2;
    mcu.r[3] = out.r3;
    mcu.r[4] = out.r4;
    mcu.r[5] = out.r5;
    mcu.r[6] = out.r6;
    mcu.sr = (uint16_t) ((mcu.sr & 0xff00u) | out.flags);
    mcu.pc = MCU_PopStack (mcu);   // 0x3126 の rts。命令数 n に含まれている

    // 時間の払い方は 2 通りある。
    //   debt: 1 ステップ 1 命令ずつ。周辺装置と割り込みの位置が完全に元のまま。
    //   まとめ: cycles を一気に進める。周辺装置は追いつき式なので結果は同じだが、
    //           割り込みの検出が最大 n 命令ぶん遅れる。音が変わらないなら速い。
    static const bool bulk = std::getenv ("SC55_BULK") != nullptr;
    if (bulk)
        mcu.cycles += 12ull * (n - 1);
    else
        mcu.native_debt = n - 1;   // 今のステップで 1 命令ぶん払う
    return true;
}

} // namespace mcu_native
