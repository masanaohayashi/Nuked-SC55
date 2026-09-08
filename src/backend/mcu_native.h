// ファームウェアの一部を、同じ結果を出すネイティブなコードに置き換える。
//
// 通常H8経路の命令解釈を、ROMごとに確認した区間単位で省く。
// 残るH8処理にもメモリアクセスや呼び出し判定の改善余地があるため、
// 区間の置き換えと合わせて、実際の音源経路で削減量を測定する。
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
#include "sc55_controller_scale.h"
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

// 00:36ee..3734: periodic TVA ramp and PCM output word. The level in r5
// has already been calculated by the firmware (including silence/saturation).
// Only ordinary SRAM under the firmware's interrupt mask is eligible.
inline bool TryAdvanceTva (mcu_t& mcu)
{
    const uint16_t voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.pc != 0x36ee
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || (voice & 1) != 0 || voice < 0x8000 || voice > 0xdfce)
        return false;

    const uint16_t oldRamp = ReadWord(mcu, voice + 6);
    const uint32_t sum = uint32_t(oldRamp) + 0x2000;
    const uint16_t ramp = sum > 0xffff ? 0xffff : uint16_t(sum);
    const uint32_t product = uint32_t(mcu.r[5]) * ramp;
    const uint16_t previous = ReadWord(mcu, voice + 0x18);
    const uint16_t now = uint16_t(product >> 16);
    const unsigned magnitude = now < previous ? unsigned(previous - now) : unsigned(now - previous);
    uint16_t calculated;
    const uint16_t command = sc55::TvaWord(mcu.r[5], ramp, previous, calculated);

    if (oldRamp != 0xffff)
        MCU_Write16(mcu, voice + 6, ramp);
    MCU_Write16(mcu, voice + 0x18, now);
    MCU_Write16(mcu, voice + 0x1a, command);
    mcu.r[2] = uint16_t(magnitude);
    mcu.r[3] = uint16_t(product);
    mcu.r[5] = magnitude <= 16 ? now : command;
    mcu.r[6] = previous;
    // MOV preserves carry from CMP(magnitude,16), or SUB(now,previous)
    // on the exact-equality shortcut. All other SR bits are preserved.
    const unsigned carry = magnitude != 0 && magnitude < 16 ? STATUS_C : 0;
    mcu.sr = uint16_t((mcu.sr & ~0x0fu) | carry | (command & 0x8000 ? STATUS_N : 0));
    mcu.pc = 0x3734;

    unsigned instructions = 9; // 36ee..36f4 and 3702..370e
    if (oldRamp != 0xffff)
        instructions += 3 + unsigned(sum > 0xffff); // add, branch, store, optional saturation
    instructions += magnitude == 0 ? 1 : 5 + (magnitude <= 16 ? 1 : 3);
    // Retain every peripheral clock step; never opt into SC55_BULK here.
    mcu.native_debt = instructions - 1;
    return true;
}

// 5c20..5ff4: all eleven per-voice controller outputs. The caller has masked
// interrupts; no peripheral register is accessed in this region. Keep the
// firmware's store order, scratch SRAM, exit registers and instruction count.
inline bool TryPrepareControllers(mcu_t& mcu)
{
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.pc != 0x5c20
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || mcu.r[1] >= 24)
        return false;
    const unsigned voice = mcu.r[0];
    if (voice != ReadWord(mcu, 0x676a + 2*mcu.r[1]))
        return false;
    const unsigned part = MCU_Read(mcu, 0xc8e4 + mcu.r[1]);
    const unsigned key = MCU_Read(mcu, 0xc8fc + mcu.r[1]);
    if (part >= 16 || key >= 128) return false;
    const uint16_t partBase = ReadWord(mcu, 0x74a4 + 2*part);
    if (partBase < 0x8000 || partBase > 0xdfa8 || voice < 0x8072 || voice > 0xdf68)
        return false;
    const uint8_t pressure = MCU_Read(mcu, 0x9740 + 128*part + key);
    MCU_Write16(mcu, 0xcb4e, partBase);
    MCU_Write(mcu, 0xcb50, pressure);
    unsigned instructions = 15; // 13 setup instructions plus r3 reload/doubling at 5c58.
    constexpr unsigned order[]{0,2,1,7,3,10,6,9,5,8,4};
    constexpr int offsets[]{0x86,0x88,0x8a,-0x72,0x90,0x8c,0x8e,-0x50,0x92,0x94,0x96};
    sc55::ControllerScaleResult scaled{};
    for (unsigned i : order)
    {
        std::array<uint16_t,5> contributions;
        for (unsigned source = 0; source < 5; ++source)
            contributions[source] = ReadWord(mcu, 0x9060 + source*0x160 + i*0x20 + part*2);
        scaled = sc55::ScaleVoiceController(i,MCU_Read(mcu, partBase+0x4c+i+(i>=3)),
                                           pressure,contributions);
        MCU_Write16(mcu, uint16_t(int(voice)+offsets[i]), scaled.value);
        instructions += scaled.instructions + unsigned(i != 0);
    }
    mcu.r[2] = partBase;
    mcu.r[3] = uint16_t(part*2);
    mcu.r[4] = scaled.value;
    mcu.r[5] = scaled.productLow;
    mcu.r[6] = uint16_t(part);
    mcu.sr = uint16_t((mcu.sr & ~0x0fu) | (scaled.carry ? STATUS_C : 0)
        | (scaled.value & 0x8000 ? STATUS_N : 0) | (scaled.value == 0 ? STATUS_Z : 0));
    mcu.pc = 0x5ff4; // Leave the final RTS to the interpreter.
    mcu.native_debt = instructions-1;
    return true;
}

// 473c..47fa: cutoff interpolation, ceiling and PCM ramp command. Stop at the
// selected RTS so stack effects remain in H8. The firmware masks interrupts
// across this SRAM-only calculation; all elapsed peripheral steps are retained.
inline bool TryAdvanceCutoff(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.pc != 0x473c
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || (voice & 1) || voice < 0x8030 || voice > 0xdf96)
        return false;
    const uint16_t control = ReadWord(mcu,voice+0x22);
    const uint8_t originalFlag = MCU_Read(mcu,voice+0x68);
    const uint16_t selector = ReadWord(mcu,voice-0x30);
    if (control > 0x7fff || originalFlag > 127 || selector > 8) return false;

    unsigned instructions = 8;
    uint16_t r4 = control;
    uint16_t level = ReadWord(mcu,0x7612+2*(control>>8));
    if (control & 255)
    {
        const uint16_t difference = uint16_t(ReadWord(mcu,0x7614+2*(control>>8))-level);
        const uint32_t product = uint32_t(control&255)*difference;
        r4 = uint16_t(product>>16);
        level = uint16_t(level+uint16_t(product>>8));
        instructions += 8;
    }
    level = uint16_t(level*2u);
    uint16_t r5 = ReadWord(mcu,voice+0x24);
    const uint8_t flag = originalFlag < 8 ? 8 : originalFlag;
    instructions += 4 + unsigned(originalFlag < 8);
    uint16_t r3 = uint16_t(MCU_Read(mcu,0x7816+flag)<<8);
    instructions += 6;
    if (level > r3) { level = r3; ++instructions; }
    instructions += 3;
    if (level > 0xe600) { level = 0xe600; ++instructions; }
    if (originalFlag < 8) MCU_Write(mcu,voice+0x68,flag);
    MCU_Write16(mcu,voice+0x24,level);
    uint16_t r2 = uint16_t(level-r5), r6 = level;
    uint16_t command = 0xff00, exit = 0x47f4;
    bool carry = false;
    instructions += 3;
    if (r2 == 0)
        ++instructions; // 47ef store
    else
    {
        ++instructions; // 4794
        if (level < r5)
        {
            r2 = uint16_t(0u-r2);
            instructions += 2;
        }
        else
        {
            r6 &= 0xff00;
            r5 &= 0xff00;
            instructions += 4;
            if (r6 == r5)
            {
                r6 = uint16_t(r6+0x100);
                instructions += 7;
                if (r6 > r3) { r6 = r3; ++instructions; }
            }
        }
        carry = r6 < 0xe600;
        instructions += 4;
        if (r6 > 0xe600) { r6 = 0xe600; ++instructions; }
        mcu.r[1] = selector;
        if (selector == 0)
        {
            r6 = uint16_t((r6 & 0xff00) | 0xaf);
            command = r6;
            exit = 0x47fa;
            instructions += 2;
        }
        else
        {
            r3 = MCU_Read(mcu,0x6b06+selector);
            instructions += 2;
            // Nonzero r2 reaches carry in at most 16 iterations.
            for (unsigned shift = 0; shift < 16; ++shift)
            {
                const bool overflow = (r2 & 0x8000) != 0;
                r2 = uint16_t(r2<<1);
                instructions += 2;
                if (overflow) break;
                ++instructions;
                if (r3-- == 0)
                {
                    r3 = 0;
                    r2 >>= 1;
                    instructions += 2;
                    break;
                }
            }
            const unsigned rounded = ((r2>>8)>>3)+1;
            carry = (rounded & 1) != 0;
            r2 = uint16_t((rounded>>1) | MCU_Read(mcu,0x67ba+r3));
            instructions += 9;
            if (r2 == 0)
                ++instructions;
            else
            {
                r6 = uint16_t((r6 & 0xff00) | r2);
                command = r6;
                exit = 0x47ee;
                instructions += 2;
            }
        }
    }
    MCU_Write16(mcu,voice+0x26,command);
    mcu.r[2] = r2; mcu.r[3] = r3; mcu.r[4] = r4; mcu.r[5] = r5; mcu.r[6] = r6;
    mcu.sr = uint16_t((mcu.sr & ~0x0fu) | (carry ? STATUS_C : 0)
        | (command & 0x8000 ? STATUS_N : 0) | (command == 0 ? STATUS_Z : 0));
    mcu.pc = exit;
    mcu.native_debt = instructions-1;
    return true;
}

// Register-accurate 312b..3187 used by v1.21 level composition. Unlike the
// legacy helper above, no process-global scratch register is used.
inline unsigned ModulateV121(uint16_t& level, uint16_t a, uint16_t b,
                            uint16_t& depth, uint16_t& high, uint16_t& low)
{
    const bool aNegative = (a & 0x8000) != 0, bNegative = (b & 0x8000) != 0;
    bool negative = aNegative;
    unsigned instructions = 4;
    uint16_t amount;
    if (aNegative == bNegative)
    {
        amount = aNegative ? uint16_t(0u-a-b) : uint16_t(a+b);
        instructions += aNegative ? 5 : 3;
        if (amount > 0x7f00) { amount = 0x7f00; instructions += 2; }
    }
    else
    {
        amount = uint16_t(a+b);
        negative = (amount & 0x8000) != 0;
        instructions += 2;
        if (negative) { amount = uint16_t(0u-amount); instructions += 2; }
    }
    const bool flip = (depth & 0x8000) != 0;
    instructions += 2;
    if (flip)
    {
        depth = uint16_t(0u-depth);
        instructions += negative ? 1 : 2;
    }
    const uint32_t product = (uint32_t(amount)*depth)<<1;
    high = uint16_t(product>>16);
    low = uint16_t(product-1);
    const unsigned delta = unsigned(high) + unsigned(uint16_t(product) != 0);
    instructions += 7; // six arithmetic/branch instructions plus RTS
    if (negative != flip)
    {
        if (level < delta) { level = 0; ++instructions; }
        else level = uint16_t(level-delta);
    }
    else level = uint16_t(level+delta);
    return instructions;
}

// 309b..312a including its two modulation subcalls. Leave the outer RTS to
// H8, but reproduce both BSR stack writes and all exit registers/flags.
inline bool TryComposeLevelV121(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0], stack = mcu.r[7];
    const bool stackRam = stack >= 0xfb82 && stack <= 0xff80
        && (mcu.dev_register[DEV_RAMCR] & 0x80);
    const bool stackSram = stack >= 0xd002 && stack <= 0xdffe;
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.pc != 0x309b
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || (voice & 1) || voice < 0x807a || voice > 0xcf68
        || (stack & 1) || (!stackRam && !stackSram) || mcu.r[1] >= 24)
        return false;
    const unsigned part = MCU_Read(mcu,0xc8e4+mcu.r[1]);
    const unsigned patch = ReadWord(mcu,voice+0x2e);
    const unsigned selector = ReadWord(mcu,voice+0x30);
    if (part >= 16 || patch < 0x8000 || patch > 0xdff7 || selector > 0xdeff)
        return false;
    uint16_t r6 = MCU_Read(mcu,0x8002);
    uint32_t product = uint32_t(MCU_Read(mcu,0xab36+part))*MCU_Read(mcu,patch+8)*r6;
    uint16_t scaled = uint16_t((product<<2)>>8);
    unsigned instructions = 17;
    if (selector)
    {
        r6 = MCU_Read(mcu,selector+0x100);
        scaled = uint16_t(((uint32_t(scaled)*r6)<<1)>>8);
        product = uint32_t(scaled)*0x830e;
        instructions += 9;
    }
    else { product = uint32_t(scaled)*0x8208; ++instructions; }
    const bool initialCarry = (product & 0x80000000u) != 0;
    product <<= 1;
    uint16_t r2 = uint16_t(product>>16), r3 = uint16_t(product), r4 = r2, r5 = 0;
    instructions += 4;
    uint16_t exit = 0x30ea;
    bool carry = initialCarry;
    if (r4 == 0) ++instructions; // CLR r5, not the outer RTS
    else
    {
        const uint16_t bias = ReadWord(mcu,voice+0x8a);
        instructions += 2;
        if (bias != 0)
        {
            ++instructions;
            if (bias & 0x8000)
            {
                const auto magnitude = uint16_t(0u-bias);
                instructions += 3;
                if (r4 < magnitude) { r4 = 0; instructions += 2; }
                else r4 = uint16_t(r4-magnitude);
            }
            else { r4 = uint16_t(r4+bias); ++instructions; }
        }
        constexpr int offsetsA[]{-122,-88}, offsetsB[]{0x8e,0x96}, depths[]{-96,-62};
        for (unsigned i = 0; i < 2; ++i)
        {
            const auto a = ReadWord(mcu,uint16_t(int(voice)+offsetsA[i]));
            const auto b = ReadWord(mcu,voice+offsetsB[i]);
            r6 = ReadWord(mcu,uint16_t(int(voice)+depths[i]));
            MCU_Write16(mcu,stack-2,i == 0 ? 0x310b : 0x3117);
            instructions += 4 + ModulateV121(r4,a,b,r6,r2,r3);
        }
        product = ((uint32_t(r4)*r4)>>16)*0x208;
        r4 = uint16_t(product>>16);
        carry = r4 < 0xff;
        instructions += 4;
        if (carry)
        {
            r5 = uint16_t(product>>8);
            exit = 0x3126;
            instructions += 2;
        }
        else { r5 = 0xffff; exit = 0x312a; ++instructions; }
    }
    mcu.r[2] = r2; mcu.r[3] = r3; mcu.r[4] = r4; mcu.r[5] = r5; mcu.r[6] = r6;
    mcu.sr = uint16_t((mcu.sr & ~0x0fu) | (carry ? STATUS_C : 0)
        | (r5 & 0x8000 ? STATUS_N : 0) | (r5 == 0 ? STATUS_Z : 0));
    mcu.pc = exit;
    mcu.native_debt = instructions-1;
    return true;
}

// Both modulation blocks share 3b2c. Delay, attack and frequency preparation
// are SRAM-only. Sine generation is folded too; other waveforms resume in H8,
// especially sample/hold where PCM readback must occur at its original cycle.
inline bool TryAdvanceLfo(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    const bool secondEntry = mcu.pc == 0x3b26;
    const unsigned block = secondEntry ? uint16_t(voice-94) : mcu.r[1];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || (!secondEntry && mcu.pc != 0x3b2c)
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0
        || (block != voice-128 && block != voice-94))
        return false;
    const unsigned rate = MCU_Read(mcu,block+12), shape = ReadWord(mcu,block+20);
    if (rate > 127 || (shape & 1) || shape > 12) return false;
    const uint16_t tick = ReadWord(mcu,0xac5a);
    unsigned instructions = (secondEntry ? 2 : 0) + 3;
    uint16_t r6 = ReadWord(mcu,block+24);
    bool delayed = false;
    if (r6 != 0xffff)
    {
        const uint32_t sum = uint32_t(ReadWord(mcu,block+16))*tick+r6;
        instructions += 6;
        if ((sum>>16) == 0)
        {
            MCU_Write16(mcu,block+24,uint16_t(sum));
            instructions += 3;
            delayed = uint16_t(sum) != 0xffff;
        }
        if (!delayed) { MCU_Write16(mcu,block+24,0xffff); ++instructions; }
    }
    if (!delayed)
    {
        r6 = ReadWord(mcu,block+26);
        instructions += 3;
        if (r6 == 0xffff)
        {
            for (unsigned i = 0; i < 3; ++i)
                MCU_Write16(mcu,block+6+i*2,ReadWord(mcu,block+i*2));
            instructions += 6;
        }
        else
        {
            const uint32_t sum = uint32_t(ReadWord(mcu,block+18))*tick+r6;
            const uint16_t attack = sum > 0xffff ? 0xffff : uint16_t(sum);
            instructions += 7 + unsigned(sum > 0xffff);
            MCU_Write16(mcu,block+26,attack);
            for (unsigned i = 0; i < 3; ++i)
            {
                const uint16_t depth = ReadWord(mcu,block+i*2);
                const bool negative = (depth & 0x8000) != 0;
                const uint16_t magnitude = negative ? uint16_t(0u-depth) : depth;
                const uint16_t high = uint16_t((uint32_t(magnitude)*attack)>>16);
                MCU_Write16(mcu,block+6+i*2,negative ? uint16_t(0u-high) : high);
                instructions += negative ? 7 : 4;
            }
            ++instructions;
        }
    }
    uint16_t r3 = ReadWord(mcu,0x7012+rate*2);
    const uint16_t modifier = ReadWord(mcu,block+14);
    uint16_t effective = uint16_t(r3+modifier);
    bool carry = effective < 0x28f6;
    instructions += 13;
    if (effective > 0x28f6)
    {
        effective = (modifier & 0x8000) ? 0 : 0x28f6;
        instructions += (modifier & 0x8000) ? 2 : 1;
    }
    const uint32_t product = uint32_t(effective)*tick;
    uint16_t r4 = uint16_t(product>>16), r5 = uint16_t(product);
    uint16_t r2 = ReadWord(mcu,0x74c4+shape), exit = r2;
    uint16_t flags = 0; // MULXU clears C; final target MOV clears N/Z/V.
    if (shape == 0)
    {
        r6 = uint16_t(r5+ReadWord(mcu,block+22));
        MCU_Write16(mcu,block+22,r6);
        r5 = r6 < 0x8000 ? uint16_t(0x8000-r6) : uint16_t(r6-0x8000);
        const unsigned index = r5>>8;
        const unsigned low = MCU_Read(mcu,0x7412+index), high = MCU_Read(mcu,0x7413+index);
        const unsigned magnitude = low > high ? low-high : high-low;
        r2 = uint16_t(magnitude*(r5&255));
        r4 = low > high ? uint16_t((low<<8)-r2) : uint16_t((low<<8)+r2);
        r4 >>= 1;
        carry = r6 < 0x8000;
        if (r6 > 0x8000) { carry = r4 != 0; r4 = uint16_t(0u-r4); }
        MCU_Write16(mcu,block+32,r4);
        flags = uint16_t((carry ? STATUS_C : 0) | (r4 & 0x8000 ? STATUS_N : 0)
            | (r4 == 0 ? STATUS_Z : 0));
        instructions += 21 + unsigned(r6 < 0x8000) + (low > high ? 2 : 0) + unsigned(r6 > 0x8000);
        exit = 0x3c30;
    }
    mcu.r[1] = uint16_t(block); mcu.r[2] = r2; mcu.r[3] = r3;
    mcu.r[4] = r4; mcu.r[5] = r5; mcu.r[6] = r6;
    mcu.sr = uint16_t((mcu.sr & ~0x0fu) | flags);
    mcu.pc = exit;
    mcu.native_debt = instructions-1;
    return true;
}

// 5368..53e4: signed depth composition and rounded modulation of 24-bit pitch.
// Leave the caller's stack and RTS to H8. All data accesses are voice SRAM.
inline bool TryModulatePitch(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.pc != 0x5368
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0)
        return false;
    const uint16_t first = mcu.r[2], second = mcu.r[3];
    bool negative = (first & 0x8000) != 0;
    uint16_t magnitude;
    unsigned instructions = 4;
    if ((first & 0x8000) == (second & 0x8000))
    {
        magnitude = negative ? uint16_t(0u-first-second) : uint16_t(first+second);
        instructions += negative ? 5 : 3;
        if (magnitude > 6000) { magnitude = 6000; instructions += 2; }
    }
    else
    {
        magnitude = uint16_t(first+second);
        negative = (magnitude & 0x8000) != 0;
        instructions += 2;
        if (negative) { magnitude = uint16_t(0u-magnitude); instructions += 2; }
    }
    uint16_t waveform = mcu.r[6];
    instructions += 2;
    if (waveform & 0x8000)
    {
        waveform = uint16_t(0u-waveform);
        instructions += negative ? 1 : 2;
        negative = !negative;
    }
    const uint32_t product = uint32_t(magnitude)*uint16_t(waveform*2u)+0x8000u;
    const uint16_t amount = uint16_t(product>>16);
    uint16_t r5 = uint16_t((mcu.r[5]&0xff00) | MCU_Read(mcu,voice+45));
    uint16_t r6 = ReadWord(mcu,voice+70);
    bool carry;
    instructions += negative ? 10 : 9;
    if (negative)
    {
        const bool borrow = r6 < amount;
        r6 = uint16_t(r6-amount);
        carry = (r5&255) < unsigned(borrow);
        r5 = uint16_t((r5&0xff00) | uint8_t(r5-unsigned(borrow)));
        if (r5 & 0x80) { r6 = 0; r5 &= 0xff00; carry = false; instructions += 2; }
    }
    else
    {
        const unsigned sum = unsigned(r6)+amount;
        r6 = uint16_t(sum);
        const unsigned high = (r5&255)+(sum>>16);
        carry = high > 255;
        r5 = uint16_t((r5&0xff00) | uint8_t(high));
    }
    MCU_Write(mcu,voice+45,uint8_t(r5));
    MCU_Write16(mcu,voice+70,r6);
    instructions += 2;
    mcu.r[2] = amount; mcu.r[3] = uint16_t(product); mcu.r[5] = r5; mcu.r[6] = r6;
    mcu.sr = uint16_t((mcu.sr & ~0x0fu) | (carry ? STATUS_C : 0)
        | (r6 & 0x8000 ? STATUS_N : 0) | (r6 == 0 ? STATUS_Z : 0));
    mcu.pc = 0x53e4;
    mcu.native_debt = instructions-1;
    return true;
}

// 51e7..527c: signed 24-bit pitch delta to PCM rate, using the original ROM
// tables. Preserve the shift-loop instruction debt, even for extreme deltas.
inline bool TryConvertPitch(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.pc != 0x51e7
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0)
        return false;
    const uint32_t input = (uint32_t(mcu.r[2]&255)<<16)|mcu.r[3];
    const uint32_t reference = (uint32_t(MCU_Read(mcu,voice+41))<<16)|ReadWord(mcu,voice+62);
    const uint32_t delta = (input-reference-12000u)&0xffffff;
    const bool negative = (delta & 0x800000) != 0;
    const uint32_t magnitude = negative ? (0x1000000u-delta) : delta;
    uint16_t remainder = uint16_t(magnitude%12000);
    uint16_t octave = uint16_t(magnitude/12000);
    uint16_t r1 = mcu.r[1], r2 = remainder, r3 = octave, r4, r5 = mcu.r[5];
    unsigned instructions = 5 + (negative ? 8 : 4);
    bool carry = false;
    if (!negative && octave != 0) { r4 = 0xffff; instructions += 2; }
    else
    {
        if (negative && remainder != 0) {
            ++octave; remainder = uint16_t(12000-remainder); instructions += 3;
        }
        const uint16_t fine = ReadWord(mcu,0x7b7a+(remainder&255)*2);
        r1 = ReadWord(mcu,0x7d7a+(remainder>>8)*2);
        const uint32_t product = uint32_t(fine)*r1;
        r5 = uint16_t(product);
        const uint16_t high = uint16_t(product>>16);
        // ROTL twice, mask low byte to 3, SWAP, then add coarse table value.
        const uint16_t rotated = uint16_t((high<<2)|(high>>14));
        const uint16_t masked = uint16_t((rotated&0xff00)|(rotated&3));
        const uint16_t correction = uint16_t((masked<<8)|(masked>>8));
        const unsigned sum = unsigned(correction)+r1;
        r4 = uint16_t(sum); carry = sum > 0xffff;
        r2 = uint16_t((remainder<<8)|(remainder>>8));
        r3 = octave;
        instructions += 15;
        if (negative)
        {
            instructions += 2;
            if (octave != 0) {
                carry = octave <= 16 && ((r4>>(octave-1))&1) != 0;
                r4 = octave < 16 ? uint16_t(r4>>octave) : 0;
                r3 = 0xffff;
                instructions += 2+2*unsigned(octave);
            }
        }
    }
    MCU_Write16(mcu,0xc8b0,r4);
    mcu.r[1] = r1; mcu.r[2] = r2; mcu.r[3] = r3; mcu.r[4] = r4; mcu.r[5] = r5;
    mcu.sr = uint16_t((mcu.sr & ~0x0fu) | (carry ? STATUS_C : 0)
        | (r4 & 0x8000 ? STATUS_N : 0) | (r4 == 0 ? STATUS_Z : 0));
    mcu.pc = 0x527c; mcu.native_debt = instructions; // includes final store
    return true;
}

// 527c..5367: refresh the source-byte-keyed pitch correction, then saturate
// the final PCM rate. RTS stays interpreted. All accesses are ROM or SRAM.
// Keep this comparatively infrequent block out of the per-instruction loop.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
inline bool TryCorrectPitch(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.pc != 0x527c
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0)
        return false;
    uint16_t r1 = ReadWord(mcu,voice+46);
    // Reject peripheral/invalid source pointers before reading or changing state.
    if (r1 < 0x8000 || r1 > 0xdff8) return false;
    uint16_t r2 = MCU_Read(mcu,r1+7), r3 = mcu.r[3], r5 = mcu.r[5];
    unsigned instructions = 5;
    if (r2 != MCU_Read(mcu,voice+0xa4))
    {
        const uint8_t source = uint8_t(r2);
        MCU_Write(mcu,voice+0xa4,source);
        const uint32_t reference = (uint32_t(MCU_Read(mcu,voice+41))<<16)|ReadWord(mcu,voice+62);
        const uint32_t delta = (reference-81000u)&0xffffff;
        const bool negative = (delta & 0x800000) != 0;
        const uint32_t magnitude = negative ? 0x1000000u-delta : delta;
        unsigned octave = magnitude/12000;
        unsigned remainder = magnitude%12000;
        uint16_t divisor;
        instructions += 6 + (negative ? 8 : 4);
        if (!negative && octave != 0) {
            divisor = 0xffff; instructions += 2;
        } else {
            if (negative && remainder != 0) {
                ++octave; remainder = 12000-remainder; instructions += 3;
            }
            const uint16_t fine = ReadWord(mcu,0x7b7a+(remainder&255)*2);
            r1 = ReadWord(mcu,0x7d7a+(remainder>>8)*2);
            const uint32_t product = uint32_t(fine)*r1;
            r5 = uint16_t(product);
            const uint16_t high = uint16_t(product>>16);
            const uint16_t rotated = uint16_t((high<<2)|(high>>14));
            const uint16_t masked = uint16_t((rotated&0xff00)|(rotated&3));
            divisor = uint16_t(uint16_t((masked<<8)|(masked>>8))+r1);
            instructions += 15;
            if (negative) {
                instructions += 2;
                if (octave != 0) {
                    divisor = octave < 16 ? uint16_t(divisor>>octave) : 0;
                    instructions += 3+2*octave; // SUB, shift/SCB, TST/BNE
                    if (divisor == 0) { divisor = 1; instructions += 2; }
                }
            }
        }
        // DIVXU overflow leaves the dividend registers intact. BGE tests N==V,
        // so both overflow and a quotient with bit 15 set saturate to 0x7fff.
        const int offset = int(source)-128;
        r2 = uint16_t(offset < 0 ? -offset : offset);
        const uint32_t dividend = uint32_t(r2)<<16;
        const uint32_t quotient = dividend/divisor;
        if (quotient <= 0xffff) r2 = uint16_t(dividend%divisor);
        r3 = uint16_t(quotient >= 0x8000 ? 0x7fff : quotient);
        instructions += 5;
        if (offset < 0) {
            r3 = uint16_t(0u-r3);
            instructions += 4 + (quotient >= 0x8000 ? 1 : 0);
        } else {
            instructions += 2 + (quotient >= 0x8000 ? 2 : 0);
        }
        MCU_Write16(mcu,voice+0xa6,r3);
        ++instructions;
    }
    const uint16_t correction = ReadWord(mcu,voice+0xa6);
    const unsigned sum = unsigned(correction)+ReadWord(mcu,0xc8b0);
    const bool carry = sum > 0xffff;
    uint16_t rate = uint16_t(sum);
    instructions += 5; // load, BMI, ADD, carry branch, final store
    if ((correction & 0x8000) == 0 && carry) {
        rate = 0xffff; instructions += 2;
    } else if ((correction & 0x8000) != 0 && !carry) {
        rate = 0; ++instructions;
    }
    MCU_Write16(mcu,voice+72,rate);
    mcu.r[1] = r1; mcu.r[2] = r2; mcu.r[3] = r3; mcu.r[4] = rate; mcu.r[5] = r5;
    mcu.sr = uint16_t((mcu.sr & ~0x0fu) | (carry ? STATUS_C : 0)
        | (rate & 0x8000 ? STATUS_N : 0) | (rate == 0 ? STATUS_Z : 0));
    mcu.pc = 0x5367; mcu.native_debt = instructions-1;
    return true;
}

// 5175..51e7: decay the signed 24-bit glide increment, then accumulate pitch.
inline bool TryAdvancePitchGlide(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.pc != 0x5175
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0)
        return false;
    uint32_t increment = (uint32_t(MCU_Read(mcu,voice+43))<<16)|ReadWord(mcu,voice+66);
    uint16_t r1 = mcu.r[1], r6 = mcu.r[6];
    uint32_t pitch = (uint32_t(MCU_Read(mcu,voice+45))<<16)|ReadWord(mcu,voice+70);
    unsigned instructions = 9;
    bool carry = false;
    if (increment != 0) {
        const unsigned source = ReadWord(mcu,voice-58);
        if (source < 0x8000 || source > 0xdfff) return false;
        const unsigned index = MCU_Read(mcu,source);
        if (index >= 128) return false;
        r1 = ReadWord(mcu,voice-2);
        r6 = ReadWord(mcu,0x7a32+index*2);
        instructions = (increment & 0xff0000 ? 3 : 5)+6;
        const bool negative = (increment & 0x800000) != 0;
        const uint32_t magnitude = negative ? (0x1000000u-increment) : increment;
        // H8 subtracts only 24 bits of the elapsed*rate product and tests bit23.
        const uint32_t difference = (magnitude-uint32_t(ReadWord(mcu,0xac5a))*r6)&0xffffff;
        const bool crossed = (difference & 0x800000) != 0;
        increment = crossed ? 0 : negative ? (0x1000000u-difference)&0xffffff : difference;
        instructions += negative ? (crossed ? 14 : 15) : (crossed ? 10 : 8);
        MCU_Write16(mcu,voice+66,uint16_t(increment));
        MCU_Write(mcu,voice+43,uint8_t(increment>>16));
        instructions += 2;
        const uint32_t sum = pitch+increment;
        carry = sum > 0xffffff;
        pitch = sum&0xffffff;
        MCU_Write(mcu,voice+45,uint8_t(pitch>>16));
        MCU_Write16(mcu,voice+70,uint16_t(pitch));
        instructions += 7;
    }
    mcu.r[1] = r1; mcu.r[2] = uint16_t(pitch>>16); mcu.r[3] = uint16_t(pitch);
    mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|(increment>>16));
    mcu.r[5] = uint16_t(increment); mcu.r[6] = r6;
    mcu.sr = uint16_t((mcu.sr & ~0x0fu) | (carry ? STATUS_C : 0)
        | (mcu.r[3]&0x8000 ? STATUS_N : 0) | (mcu.r[3] == 0 ? STATUS_Z : 0));
    mcu.pc = 0x51e7; mcu.native_debt = instructions-1;
    return true;
}

// 5060..50cf: advance pitch-EG phase, retain overshoot time, interpolate the
// rising/falling segment and update both pitch copies. Stage dispatch stays H8.
inline bool TryAdvancePitchEnvelope(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.pc != 0x5060
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0)
        return false;
    const uint16_t elapsed = uint16_t(ReadWord(mcu,0xac5a)+ReadWord(mcu,voice+22));
    const uint16_t rate = ReadWord(mcu,voice+124);
    const uint32_t progress = uint32_t(elapsed)*rate+ReadWord(mcu,voice+12);
    uint16_t r2 = uint16_t(progress>>16), r3 = uint16_t(progress);
    unsigned instructions = 22;
    MCU_Write16(mcu,voice+22,0);
    if (r2 != 0) {
        // A zero rate cannot produce a high word here. The quotient is <=65535.
        const uint32_t overshoot = progress-0xffff;
        r2 = uint16_t(overshoot%rate);
        MCU_Write16(mcu,voice+22,uint16_t(overshoot/rate));
        r3 = 0xffff;
        instructions += 5;
    }
    MCU_Write16(mcu,voice+12,r3);
    const bool falling = MCU_Read(mcu,voice-3) != 0;
    const uint16_t start = ReadWord(mcu,voice+112), target = ReadWord(mcu,voice+114);
    const uint8_t startHigh = MCU_Read(mcu,voice+106);
    const uint16_t distance = falling ? uint16_t(start-target) : uint16_t(target-start);
    const uint32_t product = uint32_t(distance)*r3;
    uint16_t r4 = uint16_t(product>>16), r5 = uint16_t(product), r6 = mcu.r[6];
    uint16_t low;
    uint8_t high;
    bool carry;
    if (falling) {
        const bool borrow = start < r4;
        low = uint16_t(start-r4); high = uint8_t(startHigh-unsigned(borrow));
        carry = startHigh == 0 && borrow;
        r5 = uint16_t((r5&0xff00)|high); r6 = low;
    } else {
        const unsigned sum = unsigned(start)+r4;
        low = uint16_t(sum); const unsigned highSum = unsigned(startHigh)+(sum>>16);
        high = uint8_t(highSum); carry = highSum > 255;
        r3 = high; r4 = low;
    }
    MCU_Write(mcu,voice+44,high); MCU_Write16(mcu,voice+68,low);
    MCU_Write(mcu,voice+45,high); MCU_Write16(mcu,voice+70,low);
    mcu.r[2] = r2; mcu.r[3] = r3; mcu.r[4] = r4; mcu.r[5] = r5; mcu.r[6] = r6;
    mcu.sr = uint16_t((mcu.sr & ~0x0fu) | (carry ? STATUS_C : 0)
        | (low&0x8000 ? STATUS_N : 0) | (low == 0 ? STATUS_Z : 0));
    mcu.pc = 0x50cf; mcu.native_debt = instructions-1;
    return true;
}

// 4fdb/4f9e: select/advance or re-enter the pitch-EG stage, including holds.
// Return at interpolation (5060), modulation (50cf), or the original RTS.
inline bool TryDispatchPitchEnvelope(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    const bool reentry = mcu.pc == 0x4f9e;
    if (!mcu.native_v121_enabled || mcu.cp != 0 || (!reentry && mcu.pc != 0x4fdb)
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0)
        return false;
    unsigned stage = ReadWord(mcu,voice+4);
    if (stage > 22 || (stage&1)) return false;
    const uint16_t phase = ReadWord(mcu,voice+12);
    unsigned instructions = 3;
    bool carry = reentry ? (mcu.sr&STATUS_C) != 0 : phase != 0xffff;
    if (reentry || phase == 0xffff) {
        if (!reentry) {
            MCU_Write16(mcu,voice+12,0);
            stage = ReadWord(mcu,0x6ac8+stage);
            MCU_Write16(mcu,voice+4,uint16_t(stage));
            instructions += 6;
            carry = false;
        }
        if (stage == 4 || stage == 6 || stage == 8 || (reentry && stage >= 10)) {
            const unsigned segment = reentry ? (stage <= 8 ? (stage-4)/2 : stage == 10 ? 2 : 3)
                                             : (stage-4)/2;
            if (reentry && segment != 0) {
                const unsigned previous = segment-1;
                MCU_Write(mcu,voice+107,MCU_Read(mcu,voice+108+previous));
                MCU_Write16(mcu,voice+114,ReadWord(mcu,voice+116+2*previous));
                instructions += segment == 3 ? 4 : 5;
            }
            const uint16_t rate = ReadWord(mcu,voice+126+2*segment);
            const uint8_t targetHigh = MCU_Read(mcu,voice+108+segment);
            const uint16_t targetLow = ReadWord(mcu,voice+116+2*segment);
            MCU_Write16(mcu,voice+124,rate);
            const uint8_t startHigh = MCU_Read(mcu,voice+107);
            const uint16_t startLow = ReadWord(mcu,voice+114);
            const bool equalHigh = targetHigh == startHigh;
            carry = equalHigh ? targetLow < startLow : targetHigh < startHigh;
            MCU_Write(mcu,voice-3,carry ? 2 : 0);
            MCU_Write(mcu,voice+106,startHigh); MCU_Write16(mcu,voice+112,startLow);
            MCU_Write(mcu,voice+107,targetHigh); MCU_Write16(mcu,voice+114,targetLow);
            mcu.r[4] = startLow;
            mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|targetHigh); mcu.r[6] = targetLow;
            instructions += (segment == 2 ? 3 : 4)+5+unsigned(equalHigh)+1+(carry ? 2 : 1)+5;
        } else ++instructions; // 5040 reloads stage
    }
    mcu.r[3] = ReadWord(mcu,0x7b62+stage);
    instructions += 2;
    mcu.pc = mcu.r[3];
    uint16_t flagsValue = mcu.r[3];
    if (mcu.pc == 0x5049) {
        MCU_Write16(mcu,voice+22,0); carry = false;
        const uint8_t high = MCU_Read(mcu,voice+107);
        const uint16_t low = ReadWord(mcu,voice+114);
        mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|high); mcu.r[4] = low;
        MCU_Write(mcu,voice+44,high); MCU_Write16(mcu,voice+68,low);
        MCU_Write(mcu,voice+45,high); MCU_Write16(mcu,voice+70,low);
        flagsValue = low; mcu.pc = 0x50cf; instructions += 8;
    }
    mcu.sr = uint16_t((mcu.sr & ~0x0fu) | (carry ? STATUS_C : 0)
        | (flagsValue&0x8000 ? STATUS_N : 0) | (flagsValue == 0 ? STATUS_Z : 0));
    mcu.native_debt = instructions-1;
    return true;
}

// 4f51..4f85/4f5c: initialise pitch-EG state before the original BSR.
// The H8 call/return and elapsed-time restoration remain at their original PCs.
inline bool TryInitialisePitchEnvelope(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.pc != 0x4f51
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0)
        return false;
    const uint16_t index = ReadWord(mcu,voice-2);
    const uint8_t flags = MCU_Read(mcu,voice-59);
    if ((flags&0x80) == 0) {
        mcu.r[1] = index; mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|flags);
        mcu.sr = uint16_t((mcu.sr & ~0x0eu)|STATUS_Z);
        mcu.pc = 0x4f5c; mcu.native_debt = 3;
        return true;
    }
    if (index >= 24) return false;
    const unsigned part = MCU_Read(mcu,0xc8e4+index);
    if (part >= 16) return false;
    mcu.r[1] = index; mcu.r[2] = uint16_t(0xab26+part);
    MCU_Write16(mcu,voice+12,0); MCU_Write(mcu,voice+0xa4,0);
    MCU_Write16(mcu,voice-58,mcu.r[2]); MCU_Write16(mcu,voice+22,0);
    mcu.r[6] = ReadWord(mcu,0xac5a);
    MCU_Write16(mcu,0xac5c,mcu.r[6]); MCU_Write16(mcu,0xac5a,1);
    mcu.sr &= uint16_t(~0x0fu);
    mcu.pc = 0x4f85; mcu.native_debt = 13;
    return true;
}

// Initialisation also runs with interrupts enabled (EP=1). Execute exactly one
// instruction boundary there; never expose the final state ahead of an IRQ.
inline bool TryStepPitchInitialisation(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.dp != 0 || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0)
        return false;
    auto nz = [&](uint16_t value, bool byte = false) {
        const unsigned data = byte ? value&255 : value;
        mcu.sr = uint16_t((mcu.sr & ~(STATUS_N|STATUS_Z|STATUS_V))
            | (data&(byte ? 0x80 : 0x8000) ? STATUS_N : 0) | (data == 0 ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x4f51: mcu.r[1] = ReadWord(mcu,voice-2); nz(mcu.r[1]); mcu.pc = 0x4f54; break;
        case 0x4f54:
            mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|MCU_Read(mcu,voice-59));
            nz(mcu.r[2],true); mcu.pc = 0x4f58; break;
        case 0x4f58: MCU_SetStatus(mcu,(mcu.r[2]&0x80) == 0,STATUS_Z); mcu.pc = 0x4f5a; break;
        case 0x4f5a: mcu.pc = (mcu.sr&STATUS_Z) ? 0x4f5c : 0x4f60; break;
        case 0x4f60: mcu.r[2] = 0; mcu.sr = uint16_t((mcu.sr&~0x0fu)|STATUS_Z); mcu.pc = 0x4f62; break;
        case 0x4f62: MCU_Write16(mcu,voice+12,0); mcu.sr = uint16_t((mcu.sr&~0x0fu)|STATUS_Z); mcu.pc = 0x4f65; break;
        case 0x4f65: MCU_Write(mcu,voice+0xa4,0); mcu.sr = uint16_t((mcu.sr&~0x0fu)|STATUS_Z); mcu.pc = 0x4f69; break;
        case 0x4f69:
            if (mcu.r[1] >= 24) return false;
            mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|MCU_Read(mcu,0xc8e4+mcu.r[1]));
            nz(mcu.r[2],true); mcu.pc = 0x4f6d; break;
        case 0x4f6d: {
            const uint16_t before = mcu.r[2]; const unsigned sum = unsigned(before)+0xab26;
            mcu.r[2] = uint16_t(sum); nz(mcu.r[2]);
            MCU_SetStatus(mcu,sum > 0xffff,STATUS_C);
            MCU_SetStatus(mcu,(~(before^0xab26)&(before^sum)&0x8000) != 0,STATUS_V);
            mcu.pc = 0x4f71; break;
        }
        case 0x4f71: MCU_Write16(mcu,voice-58,mcu.r[2]); nz(mcu.r[2]); mcu.pc = 0x4f74; break;
        case 0x4f74: MCU_Write16(mcu,voice+22,0); mcu.sr = uint16_t((mcu.sr&~0x0fu)|STATUS_Z); mcu.pc = 0x4f77; break;
        case 0x4f77: mcu.r[6] = ReadWord(mcu,0xac5a); nz(mcu.r[6]); mcu.pc = 0x4f7b; break;
        case 0x4f7b: MCU_Write16(mcu,0xac5c,mcu.r[6]); nz(mcu.r[6]); mcu.pc = 0x4f7f; break;
        case 0x4f7f: MCU_Write16(mcu,0xac5a,1); nz(1); mcu.pc = 0x4f85; break;
        default: return false;
    }
    return true;
}

// 50cf..510a: signed controller contribution, with the firmware's 0..127000
// clamp (and its 24-bit wrapping before the positive-side comparisons).
inline bool TryAdjustEnvelopePitch(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.pc != 0x50cf
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0)
        return false;
    uint32_t pitch = (uint32_t(MCU_Read(mcu,voice+45))<<16)|ReadWord(mcu,voice+70);
    uint16_t amount = ReadWord(mcu,voice+0x86);
    unsigned instructions;
    bool carry = false;
    if (amount&0x8000) {
        amount = uint16_t(0u-amount);
        const bool underflow = pitch < amount;
        pitch = underflow ? 0 : pitch-amount;
        instructions = underflow ? 13 : 10;
    } else {
        pitch = (pitch+amount)&0xffffff;
        const unsigned high = pitch>>16;
        if (high == 1) {
            carry = (pitch&0xffff) < 0xf018;
            instructions = pitch > 127000 ? 13 : 12;
            if (pitch > 127000) pitch = 127000;
        } else {
            carry = high < 1;
            instructions = high > 1 ? 14 : 11;
            if (high > 1) pitch = 127000;
        }
    }
    mcu.r[2] = amount;
    mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|(pitch>>16)); mcu.r[6] = uint16_t(pitch);
    MCU_Write(mcu,voice+45,uint8_t(pitch>>16)); MCU_Write16(mcu,voice+70,uint16_t(pitch));
    mcu.sr = uint16_t((mcu.sr & ~0x0fu) | (carry ? STATUS_C : 0)
        | (mcu.r[6]&0x8000 ? STATUS_N : 0) | (mcu.r[6] == 0 ? STATUS_Z : 0));
    mcu.pc = 0x510a; mcu.native_debt = instructions-1;
    return true;
}

// 5124..5175: global tuning followed by the selected part's tuning. Negative
// branches test the wrapped sign, not merely unsigned underflow.
inline bool TryApplyPitchTuning(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.pc != 0x5124
        || mcu.dp != 0 || mcu.ep != 0
        || (mcu.sr & (STATUS_INT_MASK | STATUS_T)) != STATUS_INT_MASK
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0)
        return false;
    const unsigned index = ReadWord(mcu,voice-2);
    if (index >= 24) return false;
    const unsigned part = MCU_Read(mcu,0xc8e4+index);
    if (part >= 16) return false;
    uint32_t pitch = (uint32_t(MCU_Read(mcu,voice+45))<<16)|ReadWord(mcu,voice+70);
    unsigned instructions = 5; // initial loads, subtraction and sign branch
    bool carry = false;
    auto apply = [&](uint16_t offset) {
        if (offset&0x8000) {
            offset = uint16_t(0u-offset);
            pitch = (pitch-offset)&0xffffff;
            instructions += 5; // NEG, SUB, SUBX, TST, BPL
            if (pitch&0x800000) { pitch = 0; instructions += 2; }
            carry = false; // TST clears C
        } else {
            const uint32_t sum = pitch+offset;
            pitch = sum&0xffffff; carry = sum > 0xffffff;
            instructions += 3; // ADD, ADDX, BRA
        }
        return offset;
    };
    apply(uint16_t(ReadWord(mcu,0x8000)-0x400));
    instructions += 5; // index, part byte, doubling, tuning table, sign branch
    mcu.r[3] = apply(ReadWord(mcu,0xab76+2*part));
    mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|(pitch>>16)); mcu.r[6] = uint16_t(pitch);
    MCU_Write(mcu,voice+45,uint8_t(pitch>>16)); MCU_Write16(mcu,voice+70,uint16_t(pitch));
    instructions += 2;
    mcu.sr = uint16_t((mcu.sr & ~0x0fu) | (carry ? STATUS_C : 0)
        | (mcu.r[6]&0x8000 ? STATUS_N : 0) | (mcu.r[6] == 0 ? STATUS_Z : 0));
    mcu.pc = 0x5175; mcu.native_debt = instructions-1;
    return true;
}

// Stage setup has an interruptible entry too. These specialised C++ steps
// preserve every observable boundary without fetching/decoding H8 opcodes.
inline bool TryStepPitchStage(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.dp != 0 || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0)
        return false;
    auto nz = [&](unsigned value, bool byte = false) {
        const unsigned data = value&(byte ? 255 : 65535);
        mcu.sr = uint16_t((mcu.sr & ~(STATUS_N|STATUS_Z|STATUS_V))
            | (data&(byte ? 0x80 : 0x8000) ? STATUS_N : 0) | (data == 0 ? STATUS_Z : 0));
    };
    auto compare = [&](unsigned left, unsigned right, bool byte = false) {
        const unsigned mask = byte ? 255 : 65535, sign = byte ? 0x80 : 0x8000;
        left &= mask; right &= mask;
        const unsigned result = (left-right)&mask;
        nz(result,byte); MCU_SetStatus(mcu,left < right,STATUS_C);
        MCU_SetStatus(mcu,((left^right)&(left^result)&sign) != 0,STATUS_V);
    };
    auto loadWord = [&](unsigned reg, unsigned at, uint16_t next) {
        mcu.r[reg] = ReadWord(mcu,at); nz(mcu.r[reg]); mcu.pc = next;
    };
    auto loadByte = [&](unsigned reg, unsigned at, uint16_t next) {
        mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|MCU_Read(mcu,at)); nz(mcu.r[reg],true); mcu.pc = next;
    };
    auto storeWord = [&](unsigned reg, unsigned at, uint16_t next) {
        MCU_Write16(mcu,at,mcu.r[reg]); nz(mcu.r[reg]); mcu.pc = next;
    };
    auto storeByte = [&](unsigned reg, unsigned at, uint16_t next) {
        MCU_Write(mcu,at,uint8_t(mcu.r[reg])); nz(mcu.r[reg],true); mcu.pc = next;
    };
    switch (mcu.pc) {
        case 0x4f9e: loadWord(3,voice+4,0x4fa1); break;
        case 0x4fa1:
            if (mcu.r[3] > 22 || (mcu.r[3]&1)) return false;
            loadWord(3,0x7b4a+mcu.r[3],0x4fa5); break;
        case 0x4fa5: case 0x4ff6: case 0x5047: mcu.pc = mcu.r[3]; break;
        case 0x4fa7: loadByte(3,voice+108,0x4faa); break;
        case 0x4faa: loadWord(4,voice+116,0x4fad); break;
        case 0x4fad: storeByte(3,voice+107,0x4fb0); break;
        case 0x4fb0: storeWord(4,voice+114,0x4fb3); break;
        case 0x4fb3: mcu.pc = 0x5003; break;
        case 0x4fb5: loadByte(3,voice+109,0x4fb8); break;
        case 0x4fb8: loadWord(4,voice+118,0x4fbb); break;
        case 0x4fbb: storeByte(3,voice+107,0x4fbe); break;
        case 0x4fbe: storeWord(4,voice+114,0x4fc1); break;
        case 0x4fc1: mcu.pc = 0x500f; break;
        case 0x4fc3: loadByte(3,voice+110,0x4fc6); break;
        case 0x4fc6: loadWord(4,voice+120,0x4fc9); break;
        case 0x4fc9: storeByte(3,voice+107,0x4fcc); break;
        case 0x4fcc: storeWord(4,voice+114,0x4fcf); break;
        case 0x4fcf: loadWord(4,voice+132,0x4fd3); break;
        case 0x4fd3: loadByte(5,voice+111,0x4fd6); break;
        case 0x4fd6: loadWord(6,voice+122,0x4fd9); break;
        case 0x4fd9: mcu.pc = 0x5019; break;
        case 0x4fdb: loadWord(3,voice+4,0x4fde); break;
        case 0x4fde: compare(ReadWord(mcu,voice+12),0xffff); mcu.pc = 0x4fe3; break;
        case 0x4fe3: mcu.pc = (mcu.sr&STATUS_Z) ? 0x4fe5 : 0x5043; break;
        case 0x4fe5: MCU_Write16(mcu,voice+12,0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x4fe8; break;
        case 0x4fe8: loadWord(3,voice+4,0x4feb); break;
        case 0x4feb:
            if (mcu.r[3] > 22 || (mcu.r[3]&1)) return false;
            loadWord(3,0x6ac8+mcu.r[3],0x4fef); break;
        case 0x4fef: storeWord(3,voice+4,0x4ff2); break;
        case 0x4ff2:
            if (mcu.r[3] > 22 || (mcu.r[3]&1)) return false;
            loadWord(3,0x7b32+mcu.r[3],0x4ff6); break;
        case 0x4ff8: loadWord(4,voice+126,0x4ffb); break;
        case 0x4ffb: loadByte(5,voice+108,0x4ffe); break;
        case 0x4ffe: loadWord(6,voice+116,0x5001); break;
        case 0x5001: mcu.pc = 0x5019; break;
        case 0x5003: loadWord(4,voice+128,0x5007); break;
        case 0x5007: loadByte(5,voice+109,0x500a); break;
        case 0x500a: loadWord(6,voice+118,0x500d); break;
        case 0x500d: mcu.pc = 0x5019; break;
        case 0x500f: loadWord(4,voice+130,0x5013); break;
        case 0x5013: loadByte(5,voice+110,0x5016); break;
        case 0x5016: loadWord(6,voice+120,0x5019); break;
        case 0x5019: storeWord(4,voice+124,0x501c); break;
        case 0x501c: loadByte(3,voice+107,0x501f); break;
        case 0x501f: loadWord(4,voice+114,0x5022); break;
        case 0x5022: compare(mcu.r[5],mcu.r[3],true); mcu.pc = 0x5024; break;
        case 0x5024: mcu.pc = (mcu.sr&STATUS_Z) ? 0x5026 : 0x5028; break;
        case 0x5026: compare(mcu.r[6],mcu.r[4]); mcu.pc = 0x5028; break;
        case 0x5028: mcu.pc = (mcu.sr&STATUS_C) ? 0x502a : 0x5030; break;
        case 0x502a: MCU_Write(mcu,voice-3,2); nz(2,true); mcu.pc = 0x502e; break;
        case 0x502e: mcu.pc = 0x5034; break;
        case 0x5030: MCU_Write(mcu,voice-3,0); nz(0,true); mcu.pc = 0x5034; break;
        case 0x5034: storeByte(3,voice+106,0x5037); break;
        case 0x5037: storeWord(4,voice+112,0x503a); break;
        case 0x503a: storeByte(5,voice+107,0x503d); break;
        case 0x503d: storeWord(6,voice+114,0x5040); break;
        case 0x5040: loadWord(3,voice+4,0x5043); break;
        case 0x5043:
            if (mcu.r[3] > 22 || (mcu.r[3]&1)) return false;
            loadWord(3,0x7b62+mcu.r[3],0x5047); break;
        case 0x5049: MCU_Write16(mcu,voice+22,0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x504c; break;
        case 0x504c: loadByte(3,voice+107,0x504f); break;
        case 0x504f: loadWord(4,voice+114,0x5052); break;
        case 0x5052: storeByte(3,voice+44,0x5055); break;
        case 0x5055: storeWord(4,voice+68,0x5058); break;
        case 0x5058: storeByte(3,voice+45,0x505b); break;
        case 0x505b: storeWord(4,voice+70,0x505e); break;
        case 0x505e: mcu.pc = 0x50cf; break;
        default: return false;
    }
    return true;
}

// Interruptible 5060..50cf. Arithmetic is specialised to this pitch-EG routine;
// no instruction fetch/decode or deferred final-state publication is used.
inline bool TryStepPitchEnvelope(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.dp != 0 || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0)
        return false;
    auto nz = [&](unsigned value, bool byte = false) {
        const unsigned data = value&(byte ? 255 : 65535);
        mcu.sr = uint16_t((mcu.sr & ~(STATUS_N|STATUS_Z|STATUS_V))
            | (data&(byte ? 0x80 : 0x8000) ? STATUS_N : 0) | (data == 0 ? STATUS_Z : 0));
    };
    auto arithmetic = [&](unsigned reg, unsigned operand, bool subtract, bool extended = false, bool byte = false) {
        const unsigned mask = byte ? 255 : 65535;
        const unsigned left = mcu.r[reg]&mask, right = operand&mask;
        const unsigned carry = extended && (mcu.sr&STATUS_C) ? 1 : 0;
        const bool previousZero = (mcu.sr&STATUS_Z) != 0;
        const unsigned result = subtract ? left-right-carry : left+right+carry;
        const int signedLeft = byte ? int(int8_t(left)) : int(int16_t(left));
        const int signedRight = byte ? int(int8_t(right)) : int(int16_t(right));
        const int signedResult = subtract ? signedLeft-signedRight-int(carry) : signedLeft+signedRight+int(carry);
        mcu.r[reg] = uint16_t((mcu.r[reg]&~mask)|(result&mask));
        nz(result,byte);
        MCU_SetStatus(mcu,subtract ? left < right+carry : result > mask,STATUS_C);
        MCU_SetStatus(mcu,signedResult < (byte ? -128 : -32768) || signedResult > (byte ? 127 : 32767),STATUS_V);
        if (extended && !subtract && !previousZero) MCU_SetStatus(mcu,false,STATUS_Z);
    };
    auto multiply = [&](unsigned reg, uint16_t value) {
        const uint32_t result = uint32_t(mcu.r[reg])*value;
        mcu.r[reg] = uint16_t(result>>16); mcu.r[reg+1] = uint16_t(result);
        mcu.sr = uint16_t((mcu.sr&~15) | (result&0x80000000u ? STATUS_N : 0) | (result == 0 ? STATUS_Z : 0));
    };
    auto load = [&](unsigned reg, unsigned at, uint16_t next) {
        mcu.r[reg] = ReadWord(mcu,at); nz(mcu.r[reg]); mcu.pc = next;
    };
    auto store = [&](unsigned reg, unsigned at, uint16_t next, bool byte = false) {
        if (byte) MCU_Write(mcu,at,uint8_t(mcu.r[reg])); else MCU_Write16(mcu,at,mcu.r[reg]);
        nz(mcu.r[reg],byte); mcu.pc = next;
    };
    switch (mcu.pc) {
        case 0x5060: load(2,0xac5a,0x5064); break;
        case 0x5064: arithmetic(2,ReadWord(mcu,voice+22),false); mcu.pc = 0x5067; break;
        case 0x5067: MCU_Write16(mcu,voice+22,0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x506a; break;
        case 0x506a: multiply(2,ReadWord(mcu,voice+124)); mcu.pc = 0x506d; break;
        case 0x506d: arithmetic(3,ReadWord(mcu,voice+12),false); mcu.pc = 0x5070; break;
        case 0x5070: arithmetic(2,0,false,true); mcu.pc = 0x5074; break;
        case 0x5074: nz(mcu.r[2]); MCU_SetStatus(mcu,false,STATUS_C); mcu.pc = 0x5076; break;
        case 0x5076: mcu.pc = (mcu.sr&STATUS_Z) ? 0x5089 : 0x5078; break;
        case 0x5078: arithmetic(3,0xffff,true); mcu.pc = 0x507c; break;
        case 0x507c: arithmetic(2,0,true,true); mcu.pc = 0x5080; break;
        case 0x5080: {
            const uint16_t divisor = ReadWord(mcu,voice+124);
            if (!divisor) return false; // Preserve the interpreter's division trap.
            const uint32_t dividend = (uint32_t(mcu.r[2])<<16)|mcu.r[3];
            const uint32_t quotient = dividend/divisor;
            if (quotient > 0xffff) mcu.sr = uint16_t((mcu.sr&~15)|STATUS_V);
            else {
                mcu.r[2] = uint16_t(dividend%divisor); mcu.r[3] = uint16_t(quotient);
                nz(quotient); MCU_SetStatus(mcu,false,STATUS_C);
            }
            mcu.pc = 0x5083; break;
        }
        case 0x5083: store(3,voice+22,0x5086); break;
        case 0x5086: mcu.r[3] = 0xffff; nz(mcu.r[3]); mcu.pc = 0x5089; break;
        case 0x5089: store(3,voice+12,0x508c); break;
        case 0x508c: nz(MCU_Read(mcu,voice-3),true); MCU_SetStatus(mcu,false,STATUS_C); mcu.pc = 0x5090; break;
        case 0x5090: mcu.pc = (mcu.sr&STATUS_Z) ? 0x5092 : 0x50b0; break;
        case 0x5092: load(4,voice+114,0x5095); break;
        case 0x5095: arithmetic(4,ReadWord(mcu,voice+112),true); mcu.pc = 0x5098; break;
        case 0x5098: multiply(4,mcu.r[3]); mcu.pc = 0x509a; break;
        case 0x509a: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x509c; break;
        case 0x509c: arithmetic(4,ReadWord(mcu,voice+112),false); mcu.pc = 0x509f; break;
        case 0x509f: arithmetic(3,MCU_Read(mcu,voice+106),false,true,true); mcu.pc = 0x50a2; break;
        case 0x50a2: store(3,voice+44,0x50a5,true); break;
        case 0x50a5: store(4,voice+68,0x50a8); break;
        case 0x50a8: store(3,voice+45,0x50ab,true); break;
        case 0x50ab: store(4,voice+70,0x50ae); break;
        case 0x50ae: mcu.pc = 0x50cf; break;
        case 0x50b0: load(4,voice+112,0x50b3); break;
        case 0x50b3: arithmetic(4,ReadWord(mcu,voice+114),true); mcu.pc = 0x50b6; break;
        case 0x50b6: multiply(4,mcu.r[3]); mcu.pc = 0x50b8; break;
        case 0x50b8: load(6,voice+112,0x50bb); break;
        case 0x50bb:
            mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|MCU_Read(mcu,voice+106));
            nz(mcu.r[5],true); mcu.pc = 0x50be; break;
        case 0x50be: arithmetic(6,mcu.r[4],true); mcu.pc = 0x50c0; break;
        case 0x50c0: arithmetic(5,0,true,true,true); mcu.pc = 0x50c3; break;
        case 0x50c3: store(5,voice+44,0x50c6,true); break;
        case 0x50c6: store(6,voice+68,0x50c9); break;
        case 0x50c9: store(5,voice+45,0x50cc,true); break;
        case 0x50cc: store(6,voice+70,0x50cf); break;
        default: return false;
    }
    return true;
}

// Pitch control call sites, returns, argument loads and elapsed restoration.
// Calls use the existing stack bus helpers, including address-error handling.
inline bool TryStepPitchConnections(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp != 0 || mcu.dp != 0 || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a != 0)
        return false;
    auto nz = [&](uint16_t value) {
        mcu.sr = uint16_t((mcu.sr & ~(STATUS_N|STATUS_Z|STATUS_V))
            | (value&0x8000 ? STATUS_N : 0) | (value == 0 ? STATUS_Z : 0));
    };
    auto load = [&](unsigned reg, unsigned at, uint16_t next) {
        mcu.r[reg] = ReadWord(mcu,at); nz(mcu.r[reg]); mcu.pc = next;
    };
    auto call = [&](uint16_t target, uint16_t returnPc) {
        mcu.pc = returnPc; MCU_PushStack(mcu,returnPc); mcu.pc = target;
    };
    switch (mcu.pc) {
        case 0x4f5c: call(0x4f9e,0x4f5e); break;
        case 0x4f5e: mcu.pc = 0x4f90; break;
        case 0x4f85: call(0x5060,0x4f88); break;
        case 0x4f88: load(6,0xac5c,0x4f8c); break;
        case 0x4f8c: MCU_Write16(mcu,0xac5a,mcu.r[6]); nz(mcu.r[6]); mcu.pc = 0x4f90; break;
        case 0x4f90: load(1,voice-2,0x4f93); break;
        case 0x4f93: {
            if (mcu.r[1] >= 24) return false;
            const uint8_t value = MCU_Read(mcu,0xcaf4+mcu.r[1]);
            mcu.sr = uint16_t((mcu.sr&~15) | (value&0x80 ? STATUS_N : 0) | (value == 0 ? STATUS_Z : 0));
            mcu.pc = 0x4f98; break;
        }
        case 0x4f98: mcu.pc = (mcu.sr&STATUS_Z) ? 0x4f9b : 0x56d9; break;
        case 0x4f9b: mcu.pc = 0x56d8; break;
        case 0x510a: load(2,voice-118,0x510d); break;
        case 0x510d: load(3,voice+144,0x5111); break;
        case 0x5111: load(6,voice-96,0x5114); break;
        case 0x5114: call(0x5368,0x5117); break;
        case 0x5117: load(2,voice-84,0x511a); break;
        case 0x511a: load(3,voice+146,0x511e); break;
        case 0x511e: load(6,voice-62,0x5121); break;
        case 0x5121: call(0x5368,0x5124); break;
        case 0x5367: case 0x53e4:
            ++mcu.pc; mcu.pc = MCU_PopStack(mcu); break;
        default: return false;
    }
    return true;
}

} // namespace mcu_native
