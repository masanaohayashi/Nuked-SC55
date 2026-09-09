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
        case 0x56d9: {
            // The CAF4 branch skips the caller's return address before 56db.
            const unsigned result = unsigned(mcu.r[7])+2;
            const bool overflow = mcu.r[7] == 0x7ffe || mcu.r[7] == 0x7fff;
            mcu.r[7] = uint16_t(result); nz(mcu.r[7]);
            MCU_SetStatus(mcu,result > 0xffff,STATUS_C);
            MCU_SetStatus(mcu,overflow,STATUS_V);
            mcu.pc = 0x56db; break;
        }
        case 0x5367: case 0x53e4: case 0x56d8:
            ++mcu.pc; mcu.pc = MCU_PopStack(mcu); break;
        default: return false;
    }
    return true;
}

// Unmasked controller/global/part adjustment. Publish one instruction's state.
inline bool TryStepPitchAdjustment(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~(STATUS_N|STATUS_Z|STATUS_V))
            | (value&(byte ? 128 : 32768) ? STATUS_N : 0) | (!value ? STATUS_Z : 0));
    };
    auto arithmetic = [&](unsigned reg, unsigned right, bool sub, bool byte = false,
                          bool extended = false, bool compare = false) {
        const unsigned mask = byte ? 255 : 65535;
        const unsigned left = mcu.r[reg]&mask;
        const unsigned carry = extended && (mcu.sr&STATUS_C) ? 1 : 0;
        const bool zero = (mcu.sr&STATUS_Z) != 0;
        const unsigned result = sub ? left-right-carry : left+right+carry;
        const int a = byte ? int(int8_t(left)) : int(int16_t(left));
        const int b = byte ? int(int8_t(right)) : int(int16_t(right));
        const int signedResult = sub ? a-b-int(carry) : a+b+int(carry);
        if (!compare) mcu.r[reg] = uint16_t((mcu.r[reg]&~mask)|(result&mask));
        nz(result,byte);
        MCU_SetStatus(mcu,sub ? left < right+carry : result > mask,STATUS_C);
        MCU_SetStatus(mcu,signedResult < (byte ? -128 : -32768)
            || signedResult > (byte ? 127 : 32767),STATUS_V);
        if (extended && !sub && !zero) MCU_SetStatus(mcu,false,STATUS_Z);
    };
    switch (mcu.pc) {
        case 0x50cf: mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|MCU_Read(mcu,voice+45)); nz(mcu.r[5],true); mcu.pc = 0x50d2; break;
        case 0x50d2: mcu.r[6] = ReadWord(mcu,voice+70); nz(mcu.r[6]); mcu.pc = 0x50d5; break;
        case 0x50d5: mcu.r[2] = ReadWord(mcu,voice+134); nz(mcu.r[2]); mcu.pc = 0x50d9; break;
        case 0x50d9: mcu.pc = (mcu.sr&STATUS_N) ? 0x50db : 0x50ea; break;
        case 0x50db: {
            const auto value = mcu.r[2]; mcu.r[2] = uint16_t(0-value); nz(mcu.r[2]);
            MCU_SetStatus(mcu,value != 0,STATUS_C); MCU_SetStatus(mcu,value == 0x8000,STATUS_V);
            mcu.pc = 0x50dd; break;
        }
        case 0x50dd: arithmetic(6,mcu.r[2],true); mcu.pc = 0x50df; break;
        case 0x50df: arithmetic(5,0,true,true,true); mcu.pc = 0x50e2; break;
        case 0x50e2: mcu.pc = (mcu.sr&STATUS_C) ? 0x50e4 : 0x5104; break;
        case 0x50e4: mcu.r[6] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x50e6; break;
        case 0x50e6: mcu.r[5] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x50e8; break;
        case 0x50e8: case 0x50fa: mcu.pc = 0x5104; break;
        case 0x50ea: arithmetic(6,mcu.r[2],false); mcu.pc = 0x50ec; break;
        case 0x50ec: arithmetic(5,0,false,true,true); mcu.pc = 0x50ef; break;
        case 0x50ef: arithmetic(5,1,true,true,false,true); mcu.pc = 0x50f1; break;
        case 0x50f1: mcu.pc = (mcu.sr&STATUS_Z) ? 0x50fc : 0x50f3; break;
        case 0x50f3: mcu.pc = (mcu.sr&STATUS_C) ? 0x5104 : 0x50f5; break;
        case 0x50f5: mcu.r[6] = 0xf018; nz(mcu.r[6]); mcu.pc = 0x50f8; break;
        case 0x50f8: mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|1); nz(1,true); mcu.pc = 0x50fa; break;
        case 0x50fc: arithmetic(6,0xf018,true,false,false,true); mcu.pc = 0x50ff; break;
        case 0x50ff: mcu.pc = (mcu.sr&(STATUS_C|STATUS_Z)) ? 0x5104 : 0x5101; break;
        case 0x5101: mcu.r[6] = 0xf018; nz(mcu.r[6]); mcu.pc = 0x5104; break;
        case 0x5104: MCU_Write(mcu,voice+45,uint8_t(mcu.r[5])); nz(mcu.r[5],true); mcu.pc = 0x5107; break;
        case 0x5107: MCU_Write16(mcu,voice+70,mcu.r[6]); nz(mcu.r[6]); mcu.pc = 0x510a; break;
        case 0x5124: mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|MCU_Read(mcu,voice+45)); nz(mcu.r[5],true); mcu.pc = 0x5127; break;
        case 0x5127: mcu.r[6] = ReadWord(mcu,voice+70); nz(mcu.r[6]); mcu.pc = 0x512a; break;
        case 0x512a: mcu.r[3] = ReadWord(mcu,0x8000); nz(mcu.r[3]); mcu.pc = 0x512e; break;
        case 0x512e: arithmetic(3,0x400,true); mcu.pc = 0x5132; break;
        case 0x5132: mcu.pc = (mcu.sr&STATUS_N) ? 0x513b : 0x5134; break;
        case 0x5134: arithmetic(6,mcu.r[3],false); mcu.pc = 0x5136; break;
        case 0x5136: arithmetic(5,0,false,true,true); mcu.pc = 0x5139; break;
        case 0x5139: mcu.pc = 0x514a; break;
        case 0x513b: case 0x5160: {
            const auto value = mcu.r[3]; mcu.r[3] = uint16_t(0-value); nz(mcu.r[3]);
            MCU_SetStatus(mcu,value != 0,STATUS_C); MCU_SetStatus(mcu,value == 0x8000,STATUS_V);
            mcu.pc = mcu.pc == 0x513b ? 0x513d : 0x5162; break;
        }
        case 0x513d: arithmetic(6,mcu.r[3],true); mcu.pc = 0x513f; break;
        case 0x513f: arithmetic(5,0,true,true,true); mcu.pc = 0x5142; break;
        case 0x5142: nz(mcu.r[5],true); MCU_SetStatus(mcu,false,STATUS_C); mcu.pc = 0x5144; break;
        case 0x5144: mcu.pc = (mcu.sr&STATUS_N) ? 0x5146 : 0x514a; break;
        case 0x5146: mcu.r[6] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x5148; break;
        case 0x5148: mcu.r[5] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x514a; break;
        case 0x514a: mcu.r[3] = ReadWord(mcu,voice-2); nz(mcu.r[3]); mcu.pc = 0x514d; break;
        case 0x514d:
            if (mcu.r[3] >= 24) return false;
            mcu.r[3] = MCU_Read(mcu,0xc8e4+mcu.r[3]); nz(mcu.r[3],true); mcu.pc = 0x5151; break;
        case 0x5151: arithmetic(3,mcu.r[3],false); mcu.pc = 0x5153; break;
        case 0x5153:
            if (mcu.r[3] >= 32 || (mcu.r[3]&1)) return false;
            mcu.r[3] = ReadWord(mcu,0xab76+mcu.r[3]); nz(mcu.r[3]); mcu.pc = 0x5157; break;
        case 0x5157: mcu.pc = (mcu.sr&STATUS_N) ? 0x5160 : 0x5159; break;
        case 0x5159: arithmetic(6,mcu.r[3],false); mcu.pc = 0x515b; break;
        case 0x515b: arithmetic(5,0,false,true,true); mcu.pc = 0x515e; break;
        case 0x515e: mcu.pc = 0x516f; break;
        case 0x5162: arithmetic(6,mcu.r[3],true); mcu.pc = 0x5164; break;
        case 0x5164: arithmetic(5,0,true,true,true); mcu.pc = 0x5167; break;
        case 0x5167: nz(mcu.r[5],true); MCU_SetStatus(mcu,false,STATUS_C); mcu.pc = 0x5169; break;
        case 0x5169: mcu.pc = (mcu.sr&STATUS_N) ? 0x516b : 0x516f; break;
        case 0x516b: mcu.r[6] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x516d; break;
        case 0x516d: mcu.r[5] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x516f; break;
        case 0x516f: MCU_Write(mcu,voice+45,uint8_t(mcu.r[5])); nz(mcu.r[5],true); mcu.pc = 0x5172; break;
        case 0x5172: MCU_Write16(mcu,voice+70,mcu.r[6]); nz(mcu.r[6]); mcu.pc = 0x5175; break;
        default: return false;
    }
    return true;
}

// Interruptible 5368..53e4 pitch modulation, without opcode fetch/decode.
inline bool TryStepPitchModulation(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~(STATUS_N|STATUS_Z|STATUS_V))
            | (value&(byte ? 128 : 32768) ? STATUS_N : 0) | (!value ? STATUS_Z : 0));
    };
    auto arithmetic = [&](unsigned reg, unsigned right, bool sub, bool byte = false,
                          bool extended = false, bool compare = false) {
        const unsigned mask = byte ? 255 : 65535;
        const unsigned left = mcu.r[reg]&mask;
        const unsigned carry = extended && (mcu.sr&STATUS_C) ? 1 : 0;
        const bool zero = (mcu.sr&STATUS_Z) != 0;
        const unsigned result = sub ? left-right-carry : left+right+carry;
        const int a = byte ? int(int8_t(left)) : int(int16_t(left));
        const int b = byte ? int(int8_t(right)) : int(int16_t(right));
        const int signedResult = sub ? a-b-int(carry) : a+b+int(carry);
        if (!compare) mcu.r[reg] = uint16_t((mcu.r[reg]&~mask)|(result&mask));
        nz(result,byte);
        MCU_SetStatus(mcu,sub ? left < right+carry : result > mask,STATUS_C);
        MCU_SetStatus(mcu,signedResult < (byte ? -128 : -32768)
            || signedResult > (byte ? 127 : 32767),STATUS_V);
        if (extended && !sub && !zero) MCU_SetStatus(mcu,false,STATUS_Z);
    };
    auto test = [&](unsigned reg, bool byte = false) { nz(mcu.r[reg],byte); MCU_SetStatus(mcu,false,STATUS_C); };
    auto negate = [&](unsigned reg) {
        const auto value = mcu.r[reg]; mcu.r[reg] = uint16_t(0-value); nz(mcu.r[reg]);
        MCU_SetStatus(mcu,value != 0,STATUS_C); MCU_SetStatus(mcu,value == 0x8000,STATUS_V);
    };
    auto multiply = [&] {
        const uint32_t value = uint32_t(mcu.r[2])*mcu.r[6];
        mcu.r[2] = uint16_t(value>>16); mcu.r[3] = uint16_t(value);
        mcu.sr = uint16_t((mcu.sr&~15) | (value&0x80000000u ? STATUS_N : 0) | (!value ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x5368: test(2); mcu.pc = 0x536a; break;
        case 0x536a: mcu.pc = (mcu.sr&STATUS_N) ? 0x537c : 0x536c; break;
        case 0x536c: test(3); mcu.pc = 0x536e; break;
        case 0x536e: mcu.pc = (mcu.sr&STATUS_N) ? 0x5390 : 0x5370; break;
        case 0x5370: arithmetic(2,mcu.r[3],false); mcu.pc = 0x5372; break;
        case 0x5372: arithmetic(2,6000,true,false,false,true); mcu.pc = 0x5375; break;
        case 0x5375: mcu.pc = (mcu.sr&(STATUS_C|STATUS_Z)) ? 0x5398 : 0x5377; break;
        case 0x5377: mcu.r[2] = 6000; nz(6000); mcu.pc = 0x537a; break;
        case 0x537a: mcu.pc = 0x5398; break;
        case 0x537c: test(3); mcu.pc = 0x537e; break;
        case 0x537e: mcu.pc = (mcu.sr&STATUS_N) ? 0x5380 : 0x5390; break;
        case 0x5380: negate(2); mcu.pc = 0x5382; break;
        case 0x5382: negate(3); mcu.pc = 0x5384; break;
        case 0x5384: arithmetic(2,mcu.r[3],false); mcu.pc = 0x5386; break;
        case 0x5386: arithmetic(2,6000,true,false,false,true); mcu.pc = 0x5389; break;
        case 0x5389: mcu.pc = (mcu.sr&(STATUS_C|STATUS_Z)) ? 0x53a0 : 0x538b; break;
        case 0x538b: mcu.r[2] = 6000; nz(6000); mcu.pc = 0x538e; break;
        case 0x538e: mcu.pc = 0x53a0; break;
        case 0x5390: arithmetic(2,mcu.r[3],false); mcu.pc = 0x5392; break;
        case 0x5392: mcu.pc = (mcu.sr&STATUS_N) ? 0x5394 : 0x5398; break;
        case 0x5394: negate(2); mcu.pc = 0x5396; break;
        case 0x5396: mcu.pc = 0x53a0; break;
        case 0x5398: test(6); mcu.pc = 0x539a; break;
        case 0x539a: mcu.pc = (mcu.sr&STATUS_N) ? 0x539c : 0x53a6; break;
        case 0x539c: negate(6); mcu.pc = 0x539e; break;
        case 0x539e: mcu.pc = 0x53bf; break;
        case 0x53a0: test(6); mcu.pc = 0x53a2; break;
        case 0x53a2: mcu.pc = (mcu.sr&STATUS_N) ? 0x53a4 : 0x53bf; break;
        case 0x53a4: negate(6); mcu.pc = 0x53a6; break;
        case 0x53a6: arithmetic(6,mcu.r[6],false); mcu.pc = 0x53a8; break;
        case 0x53a8: multiply(); mcu.pc = 0x53aa; break;
        case 0x53aa: arithmetic(3,0x8000,false); mcu.pc = 0x53ae; break;
        case 0x53ae: arithmetic(2,0,false,false,true); mcu.pc = 0x53b2; break;
        case 0x53b2: mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|MCU_Read(mcu,voice+45)); nz(mcu.r[5],true); mcu.pc = 0x53b5; break;
        case 0x53b5: mcu.r[6] = ReadWord(mcu,voice+70); nz(mcu.r[6]); mcu.pc = 0x53b8; break;
        case 0x53b8: arithmetic(6,mcu.r[2],false); mcu.pc = 0x53ba; break;
        case 0x53ba: arithmetic(5,0,false,true,true); mcu.pc = 0x53bd; break;
        case 0x53bd: mcu.pc = 0x53de; break;
        case 0x53bf: arithmetic(6,mcu.r[6],false); mcu.pc = 0x53c1; break;
        case 0x53c1: multiply(); mcu.pc = 0x53c3; break;
        case 0x53c3: arithmetic(3,0x8000,false); mcu.pc = 0x53c7; break;
        case 0x53c7: arithmetic(2,0,false,false,true); mcu.pc = 0x53cb; break;
        case 0x53cb: mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|MCU_Read(mcu,voice+45)); nz(mcu.r[5],true); mcu.pc = 0x53ce; break;
        case 0x53ce: mcu.r[6] = ReadWord(mcu,voice+70); nz(mcu.r[6]); mcu.pc = 0x53d1; break;
        case 0x53d1: arithmetic(6,mcu.r[2],true); mcu.pc = 0x53d3; break;
        case 0x53d3: arithmetic(5,0,true,true,true); mcu.pc = 0x53d6; break;
        case 0x53d6: test(5,true); mcu.pc = 0x53d8; break;
        case 0x53d8: mcu.pc = (mcu.sr&STATUS_N) ? 0x53da : 0x53de; break;
        case 0x53da: mcu.r[6] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x53dc; break;
        case 0x53dc: mcu.r[5] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x53de; break;
        case 0x53de: MCU_Write(mcu,voice+45,uint8_t(mcu.r[5])); nz(mcu.r[5],true); mcu.pc = 0x53e1; break;
        case 0x53e1: MCU_Write16(mcu,voice+70,mcu.r[6]); nz(mcu.r[6]); mcu.pc = 0x53e4; break;
        default: return false;
    }
    return true;
}

// Interruptible glide decay and pitch accumulation, 5175..51e7.
inline bool TryStepPitchGlide(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~(STATUS_N|STATUS_Z|STATUS_V))
            | (value&(byte ? 128 : 32768) ? STATUS_N : 0) | (!value ? STATUS_Z : 0));
    };
    auto arithmetic = [&](unsigned reg, unsigned right, bool sub, bool byte = false,
                          bool extended = false, bool compare = false) {
        const unsigned mask = byte ? 255 : 65535;
        const unsigned left = mcu.r[reg]&mask;
        const unsigned carry = extended && (mcu.sr&STATUS_C) ? 1 : 0;
        const bool zero = (mcu.sr&STATUS_Z) != 0;
        const unsigned result = sub ? left-right-carry : left+right+carry;
        const int a = byte ? int(int8_t(left)) : int(int16_t(left));
        const int b = byte ? int(int8_t(right)) : int(int16_t(right));
        const int signedResult = sub ? a-b-int(carry) : a+b+int(carry);
        if (!compare) mcu.r[reg] = uint16_t((mcu.r[reg]&~mask)|(result&mask));
        nz(result,byte);
        MCU_SetStatus(mcu,sub ? left < right+carry : result > mask,STATUS_C);
        MCU_SetStatus(mcu,signedResult < (byte ? -128 : -32768)
            || signedResult > (byte ? 127 : 32767),STATUS_V);
        if (extended && !sub && !zero) MCU_SetStatus(mcu,false,STATUS_Z);
    };
    auto test = [&](unsigned reg, bool byte = false) { nz(mcu.r[reg],byte); MCU_SetStatus(mcu,false,STATUS_C); };
    auto clear = [&](unsigned reg, bool byte = false) {
        mcu.r[reg] = byte ? uint16_t(mcu.r[reg]&0xff00) : 0;
        mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z);
    };
    auto negate = [&](unsigned reg, bool byte = false) {
        const unsigned mask = byte ? 255 : 65535, value = mcu.r[reg]&mask;
        mcu.r[reg] = uint16_t((mcu.r[reg]&~mask)|((0-value)&mask)); nz(mcu.r[reg],byte);
        MCU_SetStatus(mcu,value != 0,STATUS_C); MCU_SetStatus(mcu,value == (byte ? 128 : 32768),STATUS_V);
    };
    auto load = [&](unsigned reg, unsigned address, bool byte = false) {
        if (byte) mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|MCU_Read(mcu,address));
        else mcu.r[reg] = ReadWord(mcu,address);
        nz(mcu.r[reg],byte);
    };
    auto store = [&](unsigned reg, unsigned address, bool byte = false) {
        if (byte) MCU_Write(mcu,address,uint8_t(mcu.r[reg])); else MCU_Write16(mcu,address,mcu.r[reg]);
        nz(mcu.r[reg],byte);
    };
    switch (mcu.pc) {
        case 0x5175: load(5,voice+66); mcu.pc = 0x5178; break;
        case 0x5178: load(4,voice+43,true); mcu.pc = 0x517b; break;
        case 0x517b: mcu.pc = (mcu.sr&STATUS_Z) ? 0x517d : 0x518b; break;
        case 0x517d: test(5); mcu.pc = 0x517f; break;
        case 0x517f: mcu.pc = (mcu.sr&STATUS_Z) ? 0x5181 : 0x518b; break;
        case 0x5181: clear(2); mcu.pc = 0x5183; break;
        case 0x5183: load(2,voice+45,true); mcu.pc = 0x5186; break;
        case 0x5186: load(3,voice+70); mcu.pc = 0x5189; break;
        case 0x5189: mcu.pc = 0x51e7; break;
        case 0x518b: load(1,voice-2); mcu.pc = 0x518e; break;
        case 0x518e: clear(3); mcu.pc = 0x5190; break;
        case 0x5190: load(2,voice-58); mcu.pc = 0x5193; break;
        case 0x5193: if (mcu.r[2] < 0x8000 || mcu.r[2] >= 0xe000) return false; load(3,mcu.r[2],true); mcu.pc = 0x5195; break;
        case 0x5195: arithmetic(3,mcu.r[3],false); mcu.pc = 0x5197; break;
        case 0x5197: if (mcu.r[3] >= 256 || (mcu.r[3]&1)) return false; load(6,0x7a32+mcu.r[3]); mcu.pc = 0x519b; break;
        case 0x519b: load(2,0xac5a); mcu.pc = 0x519f; break;
        case 0x519f: { const uint32_t value = uint32_t(mcu.r[2])*mcu.r[6]; mcu.r[2] = uint16_t(value>>16); mcu.r[3] = uint16_t(value); mcu.sr = uint16_t((mcu.sr&~15)|(value&0x80000000u ? STATUS_N : 0)|(!value ? STATUS_Z : 0)); }; mcu.pc = 0x51a1; break;
        case 0x51a1: test(4,true); mcu.pc = 0x51a3; break;
        case 0x51a3: mcu.pc = (mcu.sr&STATUS_N) ? 0x51a5 : 0x51c3; break;
        case 0x51a5: negate(4,true); mcu.pc = 0x51a7; break;
        case 0x51a7: negate(5); mcu.pc = 0x51a9; break;
        case 0x51a9: arithmetic(4,0,true,true,true); mcu.pc = 0x51ac; break;
        case 0x51ac: arithmetic(5,mcu.r[3],true); mcu.pc = 0x51ae; break;
        case 0x51ae: arithmetic(4,mcu.r[2]&255,true,true,true); mcu.pc = 0x51b0; break;
        case 0x51b0: test(4,true); mcu.pc = 0x51b2; break;
        case 0x51b2: mcu.pc = (mcu.sr&STATUS_N) ? 0x51b4 : 0x51ba; break;
        case 0x51b4: clear(4,true); mcu.pc = 0x51b6; break;
        case 0x51b6: clear(5); mcu.pc = 0x51b8; break;
        case 0x51b8: mcu.pc = 0x51cf; break;
        case 0x51ba: negate(4,true); mcu.pc = 0x51bc; break;
        case 0x51bc: negate(5); mcu.pc = 0x51be; break;
        case 0x51be: arithmetic(4,0,true,true,true); mcu.pc = 0x51c1; break;
        case 0x51c1: mcu.pc = 0x51cf; break;
        case 0x51c3: arithmetic(5,mcu.r[3],true); mcu.pc = 0x51c5; break;
        case 0x51c5: arithmetic(4,mcu.r[2]&255,true,true,true); mcu.pc = 0x51c7; break;
        case 0x51c7: test(4,true); mcu.pc = 0x51c9; break;
        case 0x51c9: mcu.pc = (mcu.sr&STATUS_N) ? 0x51cb : 0x51cf; break;
        case 0x51cb: clear(4,true); mcu.pc = 0x51cd; break;
        case 0x51cd: clear(5); mcu.pc = 0x51cf; break;
        case 0x51cf: store(5,voice+66); mcu.pc = 0x51d2; break;
        case 0x51d2: store(4,voice+43,true); mcu.pc = 0x51d5; break;
        case 0x51d5: clear(2); mcu.pc = 0x51d7; break;
        case 0x51d7: load(3,voice+70); mcu.pc = 0x51da; break;
        case 0x51da: load(2,voice+45,true); mcu.pc = 0x51dd; break;
        case 0x51dd: arithmetic(3,mcu.r[5],false); mcu.pc = 0x51df; break;
        case 0x51df: arithmetic(2,mcu.r[4]&255,false,true,true); mcu.pc = 0x51e1; break;
        case 0x51e1: store(2,voice+45,true); mcu.pc = 0x51e4; break;
        case 0x51e4: store(3,voice+70); mcu.pc = 0x51e7; break;
        default: return false;
    }
    return true;
}

// Interruptible pitch difference to PCM rate conversion, 51e7..527c.
inline bool TryStepPitchConversion(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~(STATUS_N|STATUS_Z|STATUS_V))
            | (value&(byte ? 128 : 32768) ? STATUS_N : 0) | (!value ? STATUS_Z : 0));
    };
    auto arithmetic = [&](unsigned reg, unsigned right, bool sub, bool byte = false,
                          bool extended = false, bool compare = false) {
        const unsigned mask = byte ? 255 : 65535;
        const unsigned left = mcu.r[reg]&mask;
        const unsigned carry = extended && (mcu.sr&STATUS_C) ? 1 : 0;
        const bool zero = (mcu.sr&STATUS_Z) != 0;
        const unsigned result = sub ? left-right-carry : left+right+carry;
        const int a = byte ? int(int8_t(left)) : int(int16_t(left));
        const int b = byte ? int(int8_t(right)) : int(int16_t(right));
        const int signedResult = sub ? a-b-int(carry) : a+b+int(carry);
        if (!compare) mcu.r[reg] = uint16_t((mcu.r[reg]&~mask)|(result&mask));
        nz(result,byte);
        MCU_SetStatus(mcu,sub ? left < right+carry : result > mask,STATUS_C);
        MCU_SetStatus(mcu,signedResult < (byte ? -128 : -32768)
            || signedResult > (byte ? 127 : 32767),STATUS_V);
        if (extended && !sub && !zero) MCU_SetStatus(mcu,false,STATUS_Z);
    };
    auto clear = [&] { mcu.r[1] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); };
    auto lowByte = [&] { mcu.r[1] = uint16_t((mcu.r[1]&0xff00)|(mcu.r[2]&255)); nz(mcu.r[1],true); };
    auto swap = [&](unsigned reg) { mcu.r[reg] = uint16_t((mcu.r[reg]<<8)|(mcu.r[reg]>>8)); nz(mcu.r[reg]); };
    auto extend = [&] {
        const auto before = mcu.r[2]; mcu.r[2] = uint16_t(int8_t(before));
        nz(before); // Match the interpreter's EXTS flag calculation on the source.
    };
    auto divide = [&] {
        const uint32_t value = (uint32_t(mcu.r[2])<<16)|mcu.r[3], quotient = value/12000;
        if (quotient > 65535) mcu.sr = uint16_t((mcu.sr&~15)|STATUS_V);
        else { mcu.r[2] = uint16_t(value%12000); mcu.r[3] = uint16_t(quotient); nz(quotient); MCU_SetStatus(mcu,false,STATUS_C); }
    };
    auto multiply = [&] {
        const uint32_t value = uint32_t(mcu.r[4])*mcu.r[1];
        mcu.r[4] = uint16_t(value>>16); mcu.r[5] = uint16_t(value);
        mcu.sr = uint16_t((mcu.sr&~15)|(value&0x80000000u ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto rotate = [&] {
        const bool carry = (mcu.r[4]&0x8000) != 0;
        mcu.r[4] = uint16_t((mcu.r[4]<<1)|unsigned(carry)); nz(mcu.r[4]); MCU_SetStatus(mcu,carry,STATUS_C);
    };
    auto negate = [&] { const auto value = mcu.r[2]; mcu.r[2] = uint16_t(0-value); nz(mcu.r[2]);
        MCU_SetStatus(mcu,value != 0,STATUS_C); MCU_SetStatus(mcu,value == 0x8000,STATUS_V); };
    switch (mcu.pc) {
        case 0x51e7: arithmetic(3,ReadWord(mcu,voice+62),true); mcu.pc = 0x51ea; break;
        case 0x51ea: arithmetic(2,MCU_Read(mcu,voice+41),true,true,true); mcu.pc = 0x51ed; break;
        case 0x51ed: arithmetic(3,12000,true); mcu.pc = 0x51f1; break;
        case 0x51f1: arithmetic(2,0,true,true,true); mcu.pc = 0x51f4; break;
        case 0x51f4: mcu.pc = (mcu.sr&STATUS_N) ? 0x51f6 : 0x5245; break;
        case 0x51f6: extend(); mcu.pc = 0x51f8; break;
        case 0x51f8: mcu.r[2] = uint16_t(~mcu.r[2]); nz(mcu.r[2]); mcu.pc = 0x51fa; break;
        case 0x51fa: mcu.r[3] = uint16_t(~mcu.r[3]); nz(mcu.r[3]); mcu.pc = 0x51fc; break;
        case 0x51fc: arithmetic(3,1,false); mcu.pc = 0x51fe; break;
        case 0x51fe: arithmetic(2,0,false,false,true); mcu.pc = 0x5202; break;
        case 0x5202: divide(); mcu.pc = 0x5206; break;
        case 0x5206: arithmetic(2,0,true,false,false,true); mcu.pc = 0x5209; break;
        case 0x5209: mcu.pc = (mcu.sr&STATUS_Z) ? 0x5213 : 0x520b; break;
        case 0x520b: arithmetic(3,1,false); mcu.pc = 0x520d; break;
        case 0x520d: negate(); mcu.pc = 0x520f; break;
        case 0x520f: arithmetic(2,12000,false); mcu.pc = 0x5213; break;
        case 0x5213: clear(); mcu.pc = 0x5215; break;
        case 0x5215: lowByte(); mcu.pc = 0x5217; break;
        case 0x5217: arithmetic(1,mcu.r[1],false); mcu.pc = 0x5219; break;
        case 0x5219: if (mcu.r[1] >= 512 || (mcu.r[1]&1)) return false; mcu.r[4] = ReadWord(mcu,0x7b7a+mcu.r[1]); nz(mcu.r[4]); mcu.pc = 0x521d; break;
        case 0x521d: clear(); mcu.pc = 0x521f; break;
        case 0x521f: swap(2); mcu.pc = 0x5221; break;
        case 0x5221: lowByte(); mcu.pc = 0x5223; break;
        case 0x5223: arithmetic(1,mcu.r[1],false); mcu.pc = 0x5225; break;
        case 0x5225: if (mcu.r[1] >= 512 || (mcu.r[1]&1)) return false; mcu.r[1] = ReadWord(mcu,0x7d7a+mcu.r[1]); nz(mcu.r[1]); mcu.pc = 0x5229; break;
        case 0x5229: multiply(); mcu.pc = 0x522b; break;
        case 0x522b: rotate(); mcu.pc = 0x522d; break;
        case 0x522d: rotate(); mcu.pc = 0x522f; break;
        case 0x522f: mcu.r[4] &= 0xff03; nz(mcu.r[4],true); mcu.pc = 0x5232; break;
        case 0x5232: swap(4); mcu.pc = 0x5234; break;
        case 0x5234: arithmetic(4,mcu.r[1],false); mcu.pc = 0x5236; break;
        case 0x5236: nz(mcu.r[3]); MCU_SetStatus(mcu,false,STATUS_C); mcu.pc = 0x5238; break;
        case 0x5238: mcu.pc = (mcu.sr&STATUS_Z) ? 0x5278 : 0x523a; break;
        case 0x523a: arithmetic(3,1,true); mcu.pc = 0x523e; break;
        case 0x523e: { const bool carry = (mcu.r[4]&1) != 0; mcu.r[4] >>= 1; nz(mcu.r[4]); MCU_SetStatus(mcu,carry,STATUS_C); }; mcu.pc = 0x5240; break;
        case 0x5240: --mcu.r[3]; mcu.pc = mcu.r[3] != 0xffff ? 0x523e : 0x5243; break;
        case 0x5243: mcu.pc = 0x5278; break;
        case 0x5245: extend(); mcu.pc = 0x5247; break;
        case 0x5247: divide(); mcu.pc = 0x524b; break;
        case 0x524b: nz(mcu.r[3]); MCU_SetStatus(mcu,false,STATUS_C); mcu.pc = 0x524d; break;
        case 0x524d: mcu.pc = (mcu.sr&STATUS_Z) ? 0x5255 : 0x524f; break;
        case 0x524f: mcu.r[4] = 0xffff; nz(mcu.r[4]); mcu.pc = 0x5253; break;
        case 0x5253: mcu.pc = 0x5278; break;
        case 0x5255: clear(); mcu.pc = 0x5257; break;
        case 0x5257: lowByte(); mcu.pc = 0x5259; break;
        case 0x5259: arithmetic(1,mcu.r[1],false); mcu.pc = 0x525b; break;
        case 0x525b: if (mcu.r[1] >= 512 || (mcu.r[1]&1)) return false; mcu.r[4] = ReadWord(mcu,0x7b7a+mcu.r[1]); nz(mcu.r[4]); mcu.pc = 0x525f; break;
        case 0x525f: clear(); mcu.pc = 0x5261; break;
        case 0x5261: swap(2); mcu.pc = 0x5263; break;
        case 0x5263: lowByte(); mcu.pc = 0x5265; break;
        case 0x5265: arithmetic(1,mcu.r[1],false); mcu.pc = 0x5267; break;
        case 0x5267: if (mcu.r[1] >= 512 || (mcu.r[1]&1)) return false; mcu.r[1] = ReadWord(mcu,0x7d7a+mcu.r[1]); nz(mcu.r[1]); mcu.pc = 0x526b; break;
        case 0x526b: multiply(); mcu.pc = 0x526d; break;
        case 0x526d: rotate(); mcu.pc = 0x526f; break;
        case 0x526f: rotate(); mcu.pc = 0x5271; break;
        case 0x5271: mcu.r[4] &= 0xff03; nz(mcu.r[4],true); mcu.pc = 0x5274; break;
        case 0x5274: swap(4); mcu.pc = 0x5276; break;
        case 0x5276: arithmetic(4,mcu.r[1],false); mcu.pc = 0x5278; break;
        case 0x5278: MCU_Write16(mcu,0xc8b0,mcu.r[4]); nz(mcu.r[4]); mcu.pc = 0x527c; break;
        default: return false;
    }
    return true;
}

// Interruptible cache correction and final PCM rate, 527c..5367.
inline bool TryStepPitchCache(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~(STATUS_N|STATUS_Z|STATUS_V))
            | (value&(byte ? 128 : 32768) ? STATUS_N : 0) | (!value ? STATUS_Z : 0));
    };
    auto arithmetic = [&](unsigned reg, unsigned right, bool sub, bool byte = false,
                          bool extended = false, bool compare = false) {
        const unsigned mask = byte ? 255 : 65535;
        const unsigned left = mcu.r[reg]&mask;
        const unsigned carry = extended && (mcu.sr&STATUS_C) ? 1 : 0;
        const bool zero = (mcu.sr&STATUS_Z) != 0;
        const unsigned result = sub ? left-right-carry : left+right+carry;
        const int a = byte ? int(int8_t(left)) : int(int16_t(left));
        const int b = byte ? int(int8_t(right)) : int(int16_t(right));
        const int signedResult = sub ? a-b-int(carry) : a+b+int(carry);
        if (!compare) mcu.r[reg] = uint16_t((mcu.r[reg]&~mask)|(result&mask));
        nz(result,byte);
        MCU_SetStatus(mcu,sub ? left < right+carry : result > mask,STATUS_C);
        MCU_SetStatus(mcu,signedResult < (byte ? -128 : -32768)
            || signedResult > (byte ? 127 : 32767),STATUS_V);
        if (extended && !sub && !zero) MCU_SetStatus(mcu,false,STATUS_Z);
    };
    auto clear = [&] { mcu.r[1] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); };
    auto lowByte = [&] { mcu.r[1] = uint16_t((mcu.r[1]&0xff00)|(mcu.r[2]&255)); nz(mcu.r[1],true); };
    auto swap = [&](unsigned reg) { mcu.r[reg] = uint16_t((mcu.r[reg]<<8)|(mcu.r[reg]>>8)); nz(mcu.r[reg]); };
    auto extend = [&] {
        const auto before = mcu.r[2]; mcu.r[2] = uint16_t(int8_t(before));
        nz(before); // Match the interpreter's EXTS flag calculation on the source.
    };
    auto divide = [&](unsigned divisor = 12000) {
        const uint32_t value = (uint32_t(mcu.r[2])<<16)|mcu.r[3], quotient = value/divisor;
        if (quotient > 65535) mcu.sr = uint16_t((mcu.sr&~15)|STATUS_V);
        else { mcu.r[2] = uint16_t(value%divisor); mcu.r[3] = uint16_t(quotient); nz(quotient); MCU_SetStatus(mcu,false,STATUS_C); }
    };
    auto multiply = [&] {
        const uint32_t value = uint32_t(mcu.r[4])*mcu.r[1];
        mcu.r[4] = uint16_t(value>>16); mcu.r[5] = uint16_t(value);
        mcu.sr = uint16_t((mcu.sr&~15)|(value&0x80000000u ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto rotate = [&] {
        const bool carry = (mcu.r[4]&0x8000) != 0;
        mcu.r[4] = uint16_t((mcu.r[4]<<1)|unsigned(carry)); nz(mcu.r[4]); MCU_SetStatus(mcu,carry,STATUS_C);
    };
    auto negate = [&] { const auto value = mcu.r[2]; mcu.r[2] = uint16_t(0-value); nz(mcu.r[2]);
        MCU_SetStatus(mcu,value != 0,STATUS_C); MCU_SetStatus(mcu,value == 0x8000,STATUS_V); };
    switch (mcu.pc) {
        case 0x529e: extend(); mcu.pc = 0x52a0; break;
        case 0x52a0: mcu.r[2] = uint16_t(~mcu.r[2]); nz(mcu.r[2]); mcu.pc = 0x52a2; break;
        case 0x52a2: mcu.r[3] = uint16_t(~mcu.r[3]); nz(mcu.r[3]); mcu.pc = 0x52a4; break;
        case 0x52a4: arithmetic(3,1,false); mcu.pc = 0x52a6; break;
        case 0x52a6: arithmetic(2,0,false,false,true); mcu.pc = 0x52aa; break;
        case 0x52aa: divide(); mcu.pc = 0x52ae; break;
        case 0x52ae: nz(mcu.r[2]); MCU_SetStatus(mcu,false,STATUS_C); mcu.pc = 0x52b0; break;
        case 0x52b0: mcu.pc = (mcu.sr&STATUS_Z) ? 0x52ba : 0x52b2; break;
        case 0x52b2: arithmetic(3,1,false); mcu.pc = 0x52b4; break;
        case 0x52b4: negate(); mcu.pc = 0x52b6; break;
        case 0x52b6: arithmetic(2,12000,false); mcu.pc = 0x52ba; break;
        case 0x52ba: clear(); mcu.pc = 0x52bc; break;
        case 0x52bc: lowByte(); mcu.pc = 0x52be; break;
        case 0x52be: arithmetic(1,mcu.r[1],false); mcu.pc = 0x52c0; break;
        case 0x52c0: if (mcu.r[1] >= 512 || (mcu.r[1]&1)) return false; mcu.r[4] = ReadWord(mcu,0x7b7a+mcu.r[1]); nz(mcu.r[4]); mcu.pc = 0x52c4; break;
        case 0x52c4: clear(); mcu.pc = 0x52c6; break;
        case 0x52c6: swap(2); mcu.pc = 0x52c8; break;
        case 0x52c8: lowByte(); mcu.pc = 0x52ca; break;
        case 0x52ca: arithmetic(1,mcu.r[1],false); mcu.pc = 0x52cc; break;
        case 0x52cc: if (mcu.r[1] >= 512 || (mcu.r[1]&1)) return false; mcu.r[1] = ReadWord(mcu,0x7d7a+mcu.r[1]); nz(mcu.r[1]); mcu.pc = 0x52d0; break;
        case 0x52d0: multiply(); mcu.pc = 0x52d2; break;
        case 0x52d2: rotate(); mcu.pc = 0x52d4; break;
        case 0x52d4: rotate(); mcu.pc = 0x52d6; break;
        case 0x52d6: mcu.r[4] &= 0xff03; nz(mcu.r[4],true); mcu.pc = 0x52d9; break;
        case 0x52d9: swap(4); mcu.pc = 0x52db; break;
        case 0x52db: arithmetic(4,mcu.r[1],false); mcu.pc = 0x52dd; break;
        case 0x52dd: nz(mcu.r[3]); MCU_SetStatus(mcu,false,STATUS_C); mcu.pc = 0x52df; break;
        case 0x52df: mcu.pc = (mcu.sr&STATUS_Z) ? 0x5326 : 0x52e1; break;
        case 0x52e1: arithmetic(3,1,true); mcu.pc = 0x52e5; break;
        case 0x52e5: { const bool carry = (mcu.r[4]&1) != 0; mcu.r[4] >>= 1; nz(mcu.r[4]); MCU_SetStatus(mcu,carry,STATUS_C); }; mcu.pc = 0x52e7; break;
        case 0x52e7: --mcu.r[3]; mcu.pc = mcu.r[3] != 0xffff ? 0x52e5 : 0x52ea; break;
        case 0x52ea: nz(mcu.r[4]); MCU_SetStatus(mcu,false,STATUS_C); mcu.pc = 0x52ec; break;
        case 0x52f3: extend(); mcu.pc = 0x52f5; break;
        case 0x52f5: divide(); mcu.pc = 0x52f9; break;
        case 0x52f9: nz(mcu.r[3]); MCU_SetStatus(mcu,false,STATUS_C); mcu.pc = 0x52fb; break;
        case 0x52fb: mcu.pc = (mcu.sr&STATUS_Z) ? 0x5303 : 0x52fd; break;
        case 0x52fd: mcu.r[4] = 0xffff; nz(mcu.r[4]); mcu.pc = 0x5301; break;
        case 0x5301: mcu.pc = 0x5326; break;
        case 0x5303: clear(); mcu.pc = 0x5305; break;
        case 0x5305: lowByte(); mcu.pc = 0x5307; break;
        case 0x5307: arithmetic(1,mcu.r[1],false); mcu.pc = 0x5309; break;
        case 0x5309: if (mcu.r[1] >= 512 || (mcu.r[1]&1)) return false; mcu.r[4] = ReadWord(mcu,0x7b7a+mcu.r[1]); nz(mcu.r[4]); mcu.pc = 0x530d; break;
        case 0x530d: clear(); mcu.pc = 0x530f; break;
        case 0x530f: swap(2); mcu.pc = 0x5311; break;
        case 0x5311: lowByte(); mcu.pc = 0x5313; break;
        case 0x5313: arithmetic(1,mcu.r[1],false); mcu.pc = 0x5315; break;
        case 0x5315: if (mcu.r[1] >= 512 || (mcu.r[1]&1)) return false; mcu.r[1] = ReadWord(mcu,0x7d7a+mcu.r[1]); nz(mcu.r[1]); mcu.pc = 0x5319; break;
        case 0x5319: multiply(); mcu.pc = 0x531b; break;
        case 0x531b: rotate(); mcu.pc = 0x531d; break;
        case 0x531d: rotate(); mcu.pc = 0x531f; break;
        case 0x531f: mcu.r[4] &= 0xff03; nz(mcu.r[4],true); mcu.pc = 0x5322; break;
        case 0x5322: swap(4); mcu.pc = 0x5324; break;
        case 0x5324: arithmetic(4,mcu.r[1],false); mcu.pc = 0x5326; break;
        case 0x527c: mcu.r[1] = ReadWord(mcu,voice+46); nz(mcu.r[1]); mcu.pc = 0x527f; break;
        case 0x527f: mcu.r[2] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x5281; break;
        case 0x5281: if (mcu.r[1] < 0x8000 || mcu.r[1] > 0xdff8) return false; mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|MCU_Read(mcu,mcu.r[1]+7)); nz(mcu.r[2],true); mcu.pc = 0x5284; break;
        case 0x5284: arithmetic(2,MCU_Read(mcu,voice+164),true,true,false,true); mcu.pc = 0x5288; break;
        case 0x5288: mcu.pc = (mcu.sr&STATUS_Z) ? 0x534b : 0x528b; break;
        case 0x528b: MCU_Write(mcu,voice+164,uint8_t(mcu.r[2])); nz(mcu.r[2],true); mcu.pc = 0x528f; break;
        case 0x528f: mcu.r[3] = ReadWord(mcu,voice+62); nz(mcu.r[3]); mcu.pc = 0x5292; break;
        case 0x5292: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|MCU_Read(mcu,voice+41)); nz(mcu.r[2],true); mcu.pc = 0x5295; break;
        case 0x5295: arithmetic(3,0x3c68,true); mcu.pc = 0x5299; break;
        case 0x5299: arithmetic(2,1,true,true,true); mcu.pc = 0x529c; break;
        case 0x529c: mcu.pc = (mcu.sr&STATUS_N) ? 0x529e : 0x52f3; break;
        case 0x52ec: mcu.pc = (mcu.sr&STATUS_Z) ? 0x52ee : 0x5326; break;
        case 0x52ee: mcu.r[4] = 1; nz(1); mcu.pc = 0x52f1; break;
        case 0x52f1: mcu.pc = 0x5326; break;
        case 0x5326: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x5328; break;
        case 0x5328: mcu.r[2] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x532a; break;
        case 0x532a: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|MCU_Read(mcu,voice+164)); nz(mcu.r[2],true); mcu.pc = 0x532e; break;
        case 0x532e: arithmetic(2,128,true,true); mcu.pc = 0x5331; break;
        case 0x5331: mcu.pc = (mcu.sr&STATUS_N) ? 0x533c : 0x5333; break;
        case 0x5333: if (!mcu.r[4]) return false; divide(mcu.r[4]); mcu.pc = 0x5335; break;
        case 0x5335: mcu.pc = bool(mcu.sr&STATUS_N) == bool(mcu.sr&STATUS_V) ? 0x5347 : 0x5337; break;
        case 0x5337: mcu.r[3] = 0x7fff; nz(mcu.r[3]); mcu.pc = 0x533a; break;
        case 0x533a: mcu.pc = 0x5347; break;
        case 0x533c: { const unsigned value = mcu.r[2]&255; mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|((0-value)&255)); nz(mcu.r[2],true); MCU_SetStatus(mcu,value != 0,STATUS_C); MCU_SetStatus(mcu,value == 128,STATUS_V); }; mcu.pc = 0x533e; break;
        case 0x533e: if (!mcu.r[4]) return false; divide(mcu.r[4]); mcu.pc = 0x5340; break;
        case 0x5340: mcu.pc = bool(mcu.sr&STATUS_N) == bool(mcu.sr&STATUS_V) ? 0x5345 : 0x5342; break;
        case 0x5342: mcu.r[3] = 0x7fff; nz(mcu.r[3]); mcu.pc = 0x5345; break;
        case 0x5345: { const auto value = mcu.r[3]; mcu.r[3] = uint16_t(0-value); nz(mcu.r[3]); MCU_SetStatus(mcu,value != 0,STATUS_C); MCU_SetStatus(mcu,value == 0x8000,STATUS_V); }; mcu.pc = 0x5347; break;
        case 0x5347: MCU_Write16(mcu,voice+166,mcu.r[3]); nz(mcu.r[3]); mcu.pc = 0x534b; break;
        case 0x534b: mcu.r[4] = ReadWord(mcu,voice+166); nz(mcu.r[4]); mcu.pc = 0x534f; break;
        case 0x534f: mcu.pc = (mcu.sr&STATUS_N) ? 0x535c : 0x5351; break;
        case 0x5351: arithmetic(4,ReadWord(mcu,0xc8b0),false); mcu.pc = 0x5355; break;
        case 0x5355: mcu.pc = (mcu.sr&STATUS_C) ? 0x5357 : 0x5364; break;
        case 0x5357: mcu.r[4] = 0xffff; nz(mcu.r[4]); mcu.pc = 0x535a; break;
        case 0x535a: mcu.pc = 0x5364; break;
        case 0x535c: arithmetic(4,ReadWord(mcu,0xc8b0),false); mcu.pc = 0x5360; break;
        case 0x5360: mcu.pc = (mcu.sr&STATUS_C) ? 0x5364 : 0x5362; break;
        case 0x5362: mcu.r[4] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x5364; break;
        case 0x5364: MCU_Write16(mcu,voice+72,mcu.r[4]); nz(mcu.r[4]); mcu.pc = 0x5367; break;
        default: return false;
    }
    return true;
}

// TVA interpolation, 35db..365d. Only batch while IRQs are masked.
inline bool TryInterpolateTva(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || mcu.pc != 0x35db
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    const uint16_t phase = mcu.r[3];
    mcu.r[5] = uint16_t(MCU_Read(mcu,voice+96)<<8);
    mcu.r[6] = uint16_t(MCU_Read(mcu,voice+97)<<8);
    auto multiply = [&](unsigned value) {
        const uint32_t result = uint32_t(mcu.r[2])*value;
        mcu.r[2] = uint16_t(result>>16); mcu.r[3] = uint16_t(result);
    };
    auto finish = [&](unsigned right, bool subtract, unsigned count) {
        const unsigned left = mcu.r[2];
        const unsigned result = subtract ? left-right : left+right;
        const int signedResult = subtract ? int(int16_t(left))-int(int16_t(right))
                                          : int(int16_t(left))+int(int16_t(right));
        mcu.r[2] = uint16_t(result);
        mcu.sr = uint16_t((mcu.sr&~15) | (mcu.r[2]&0x8000 ? STATUS_N : 0)
            | (!mcu.r[2] ? STATUS_Z : 0)
            | ((subtract ? left < right : result > 65535) ? STATUS_C : 0)
            | (signedResult < -32768 || signedResult > 32767 ? STATUS_V : 0));
        mcu.pc = 0x365d; mcu.native_debt = count-1;
    };
    const bool falling = mcu.r[6] < mcu.r[5];
    if (!MCU_Read(mcu,voice-8)) {
        mcu.r[2] = uint16_t(mcu.r[6]-mcu.r[5]);
        if (falling) {
            mcu.r[2] = uint16_t(0-mcu.r[2]); multiply(phase);
            const auto product = mcu.r[2]; mcu.r[2] = mcu.r[5]; mcu.r[5] = product;
            finish(mcu.r[5],true,16);
        } else {
            multiply(phase); finish(mcu.r[5],false,14);
        }
        return true;
    }
    mcu.r[1] = uint16_t(mcu.r[6]-mcu.r[5]);
    if (falling) mcu.r[1] = uint16_t(0-mcu.r[1]);
    const uint16_t invertedPhase = uint16_t(~phase);
    mcu.r[2] = invertedPhase&255;
    const unsigned index = (invertedPhase>>8)*2;
    const auto base = ReadWord(mcu,0x6d10+index);
    if (falling) mcu.r[4] = base; else mcu.r[6] = base;
    mcu.r[3] = uint16_t(ReadWord(mcu,0x6d12+index)-base);
    multiply(mcu.r[3]);
    mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|(mcu.r[2]&255));
    mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8));
    mcu.r[3] = uint16_t(mcu.r[3]+base);
    if (falling) {
        mcu.r[2] = mcu.r[3]; multiply(mcu.r[1]); finish(mcu.r[6],false,30);
    } else {
        mcu.r[3] = uint16_t(~mcu.r[3]); mcu.r[2] = mcu.r[1];
        multiply(mcu.r[3]); finish(mcu.r[5],false,29);
    }
    return true;
}

// Encode the TVA target delta, 365d..36a7/36ad (RTS remains a separate step).
inline bool TryEncodeTvaTarget(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    const bool continuation = mcu.pc == 0x3666;
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || (mcu.pc != 0x365d && !continuation)
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    if (continuation && mcu.r[3] > 10) return false;
    const unsigned target = mcu.r[2], previous = continuation ? mcu.r[6] : ReadWord(mcu,voice+28);
    if (!continuation) {
        mcu.r[6] = uint16_t(previous); MCU_Write16(mcu,voice+28,uint16_t(target)); mcu.r[3] = 7;
    }
    mcu.r[5] = uint16_t(target); mcu.r[2] = uint16_t(target-previous);
    unsigned count = continuation ? 3 : 6;
    bool carry = false;
    auto finish = [&](bool noChange) {
        const uint16_t output = noChange ? 0xff00 : mcu.r[5];
        MCU_Write16(mcu,voice+30,output);
        mcu.sr = uint16_t((mcu.sr&~15) | (output&0x8000 ? STATUS_N : 0)
            | (!output ? STATUS_Z : 0) | (carry ? STATUS_C : 0));
        mcu.pc = noChange ? 0x36ad : 0x36a7;
        mcu.native_debt = count; // final store adds one instruction
    };
    if (!mcu.r[2]) { finish(true); return true; }
    ++count; // BCC
    if (target < previous) {
        mcu.r[2] = uint16_t(0-mcu.r[2]); count += 2;
    } else {
        mcu.r[6] &= 0xff00; mcu.r[5] &= 0xff00; count += 4;
        if (mcu.r[6] == mcu.r[5]) {
            const unsigned rounded = unsigned(mcu.r[5])+256;
            mcu.r[5] = uint16_t(rounded); count += 2;
            if (rounded > 65535) { mcu.r[5] = 0xff00; ++count; }
        }
    }
    for (;;) {
        const bool high = (mcu.r[2]&0x8000) != 0;
        mcu.r[2] = uint16_t(mcu.r[2]<<1); count += 2;
        if (high) break;
        ++count; // SCB/eq: decrement only when Z is clear; flags unchanged.
        if (mcu.r[2] != 0 && --mcu.r[3] != 0xffff) continue;
        mcu.r[3] = 0; mcu.r[2] >>= 1; count += 2;
        break;
    }
    mcu.r[2] >>= 8; // CLR.B then SWAP.B
    mcu.r[2] >>= 3;
    ++mcu.r[2]; carry = (mcu.r[2]&1) != 0; mcu.r[2] >>= 1;
    mcu.r[2] |= MCU_Read(mcu,0x67ba+mcu.r[3]);
    count += 9;
    if (!mcu.r[2]) { finish(true); return true; }
    mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|mcu.r[2]); ++count;
    finish(false);
    return true;
}

// TVA phase/rate advancement, 358e..35db/365d or the short-duration branches.
inline bool TryAdvanceTvaPhase(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0], duration = mcu.r[6];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || mcu.pc != 0x358e
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    if (!duration) {
        mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z);
        mcu.pc = 0x36c6; mcu.native_debt = 1; return true;
    }
    if (duration <= 8) {
        mcu.sr = uint16_t((mcu.sr&~15)|(duration == 8 ? STATUS_Z : STATUS_N|STATUS_C));
        mcu.pc = 0x36ae; mcu.native_debt = 3; return true;
    }
    const unsigned rate = 0x80000/duration;
    const unsigned elapsed = uint16_t(ReadWord(mcu,0xac5a)+ReadWord(mcu,voice+18));
    const uint32_t phase = elapsed*rate+ReadWord(mcu,voice+8);
    MCU_Write16(mcu,voice+18,0);
    mcu.r[6] = uint16_t(rate);
    mcu.r[2] = uint16_t(phase>>16); mcu.r[3] = uint16_t(phase);
    unsigned count = 19;
    if (mcu.r[2]) {
        const uint32_t excess = phase-65535;
        mcu.r[2] = uint16_t(excess%rate);
        MCU_Write16(mcu,voice+18,uint16_t(excess/rate));
        mcu.r[3] = 0xffff; count += 5;
    }
    MCU_Write16(mcu,voice+8,mcu.r[3]);
    if (mcu.r[3] == 0xffff) {
        mcu.r[2] = uint16_t(MCU_Read(mcu,voice+97)<<8);
        mcu.sr = uint16_t((mcu.sr&~15)|(mcu.r[2]&0x8000 ? STATUS_N : 0)|(!mcu.r[2] ? STATUS_Z : 0));
        mcu.pc = 0x365d; count += 4;
    } else {
        const uint16_t difference = uint16_t(mcu.r[3]+1);
        mcu.sr = uint16_t((mcu.sr&~15)|STATUS_C|(difference&0x8000 ? STATUS_N : 0)
            |(mcu.r[3] == 0x7fff ? STATUS_V : 0));
        mcu.pc = 0x35db;
    }
    mcu.native_debt = count-1;
    return true;
}

// Three TVA duration paths: attack/decay/release controller and scale factors.
inline bool TryComputeTvaDuration(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    const bool attack = mcu.pc == 0x348c, decay = mcu.pc == 0x34e2, release = mcu.pc == 0x3536;
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || !(attack||decay||release)
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    if (release && !ReadWord(mcu,voice+28)) {
        mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc = 0x3472;
        mcu.native_debt = 1; return true;
    }
    const unsigned source = ReadWord(mcu,voice+46), offset = attack ? 20 : decay ? 21 : 22;
    if (source < 0x8000 || source+offset >= 0xe000) return false;
    const unsigned control = MCU_Read(mcu,source+offset), base = MCU_Read(mcu,voice+79);
    unsigned adjusted, count = release ? 8 : 6;
    if (control < 64) {
        const unsigned subtract = 2*(64-control);
        adjusted = uint8_t(base-subtract); count += 4;
        if (base < subtract) { adjusted = 0; count += 2; }
    } else {
        adjusted = uint8_t(base+uint8_t(2*(control-64))); count += 3;
        if (adjusted&128) { adjusted = 127; ++count; }
    }
    if (adjusted >= 128) return false;
    const unsigned scale1 = ReadWord(mcu,voice-(release ? 32 : 34));
    const unsigned scale2 = ReadWord(mcu,voice-(attack ? 30 : 28));
    const uint32_t first = uint32_t(ReadWord(mcu,0x6f12+2*adjusted))*scale1;
    uint16_t value;
    count += 5; // index, table load, MUL, CMP, BCS
    if ((first>>16) >= 255) { value = 0xffff; count += 2; }
    else { value = uint16_t(first>>8); count += 3; }
    const uint32_t second = uint32_t(value)*scale2;
    mcu.r[2] = uint16_t(second>>16); mcu.r[3] = uint16_t(second); count += 3;
    const bool saturated = mcu.r[2] >= 255;
    if (saturated) { mcu.r[6] = 0xffff; count += 2; }
    else {
        mcu.r[3] = uint16_t(second>>8); mcu.r[2] = mcu.r[3]; mcu.r[6] = mcu.r[2];
        count += release ? 4 : 5;
    }
    mcu.sr = uint16_t((mcu.sr&~15)|(mcu.r[6]&0x8000 ? STATUS_N : 0)
        |(!mcu.r[6] ? STATUS_Z : 0)|(!saturated ? STATUS_C : 0));
    mcu.pc = 0x358e; mcu.native_debt = count-1;
    return true;
}

// TVA stage dispatch and three target/rate setups, 33f4..3449.
inline bool TryDispatchTvaStage(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || mcu.pc != 0x33f4
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    unsigned stage = ReadWord(mcu,voice);
    if (stage > 22 || (stage&1)) return false;
    const bool advance = ReadWord(mcu,voice+8) == 0xffff;
    unsigned setup = 0x3445, count = 6;
    if (advance) {
        stage = ReadWord(mcu,0x6ac8+stage);
        if (stage > 22 || (stage&1)) return false;
        setup = ReadWord(mcu,0x6ae0+stage); count = 10;
        if (setup != 0x3414 && setup != 0x341f && setup != 0x342a
            && setup != 0x3442 && setup != 0x3472) return false;
    }
    unsigned target = 0x3472;
    if (setup != 0x3472) {
        if (stage > 12) return false;
        target = ReadWord(mcu,0x6af8+stage);
        if (target != 0x344b && target != 0x348c && target != 0x34e2
            && target != 0x3477 && target != 0x3536) return false;
    }
    mcu.r[1] = ReadWord(mcu,voice-2);
    if (advance) {
        MCU_Write16(mcu,voice+8,0); MCU_Write16(mcu,voice,uint16_t(stage));
        if (setup == 0x3414 || setup == 0x341f || setup == 0x342a) {
            const unsigned index = setup == 0x3414 ? 0 : setup == 0x341f ? 1 : 2;
            MCU_Write(mcu,voice+79,MCU_Read(mcu,voice+80+index));
            MCU_Write(mcu,voice-8,MCU_Read(mcu,voice-7+index));
            mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|MCU_Read(mcu,voice+97));
            mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|MCU_Read(mcu,voice+98+index));
            MCU_Write(mcu,voice+96,uint8_t(mcu.r[4]));
            MCU_Write(mcu,voice+97,uint8_t(mcu.r[6]));
            count += index == 2 ? 11 : 12;
        } else if (setup == 0x3442) count += 3;
    }
    mcu.r[3] = uint16_t(target);
    mcu.sr = uint16_t((mcu.sr&~15)|(advance ? 0 : STATUS_C));
    mcu.pc = uint16_t(target); mcu.native_debt = count-1;
    return true;
}

// TVA hold and short-duration targets; return instructions are left separate.
inline bool TrySetTvaImmediate(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep
        || (mcu.pc != 0x3477 && mcu.pc != 0x36ae && mcu.pc != 0x36c6)
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    const uint16_t target = uint16_t(MCU_Read(mcu,voice+97)<<8);
    if (mcu.pc == 0x3477) {
        MCU_Write16(mcu,voice+18,0); MCU_Write16(mcu,voice+30,0xff00);
        mcu.r[6] = target; MCU_Write16(mcu,voice+28,target);
        mcu.sr = uint16_t((mcu.sr&~15)|(target&0x8000 ? STATUS_N : 0)|(!target ? STATUS_Z : 0));
        mcu.pc = target ? 0x348b : 0x3472; mcu.native_debt = 6;
    } else if (mcu.pc == 0x36c6) {
        mcu.r[2] = uint16_t(target|0xaf);
        MCU_Write16(mcu,voice+28,target); MCU_Write16(mcu,voice+30,mcu.r[2]);
        MCU_Write16(mcu,voice+8,0xffff);
        mcu.sr = uint16_t((mcu.sr&~15)|STATUS_N); mcu.pc = 0x36da; mcu.native_debt = 6;
    } else {
        if (mcu.r[3] > 8) return false;
        const auto exponent = MCU_Read(mcu,0x6b06+mcu.r[3]);
        if (exponent > 10) return false;
        mcu.r[3] = exponent; mcu.r[2] = target;
        mcu.r[6] = ReadWord(mcu,voice+28); MCU_Write16(mcu,voice+28,target);
        MCU_Write16(mcu,voice+8,0xffff);
        mcu.sr = uint16_t((mcu.sr&~15)|STATUS_N); mcu.pc = 0x3666; mcu.native_debt = 7;
    }
    return true;
}

// TVA delay counter, 344b..346b. Stop before stack/unmask/return operations.
inline bool TryAdvanceTvaDelay(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || mcu.pc != 0x344b
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    const auto index = ReadWord(mcu,voice-2);
    if (index >= 24) return false;
    const unsigned sum = unsigned(ReadWord(mcu,voice+14))+ReadWord(mcu,voice+16);
    if (sum > 65535) {
        // The three MOV.W #2 instructions are four bytes each (not five).
        MCU_Write16(mcu,voice,2); MCU_Write16(mcu,voice+2,2); MCU_Write16(mcu,voice+4,2);
    }
    MCU_Write16(mcu,voice+14,uint16_t(sum));
    mcu.r[3] = index; MCU_Write(mcu,0xac42+index,0);
    mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z);
    mcu.pc = 0x346b; mcu.native_debt = sum > 65535 ? 9 : 6;
    return true;
}

// Keep stack operations and interrupt unmasking at their original boundaries.
inline bool TryStepTvaExit(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    switch (mcu.pc) {
        case 0x346b: case 0x3472: {
            const unsigned value = mcu.r[7], sum = value+2;
            mcu.r[7] = uint16_t(sum);
            mcu.sr = uint16_t((mcu.sr&~15)|(mcu.r[7]&0x8000 ? STATUS_N : 0)
                |(!mcu.r[7] ? STATUS_Z : 0)|(sum > 65535 ? STATUS_C : 0)
                |(value == 0x7ffe || value == 0x7fff ? STATUS_V : 0));
            mcu.pc += 2; break;
        }
        case 0x346d:
            MCU_ControlRegisterWrite(mcu,0,MCU_Operand_Size::WORD,mcu.sr&0xf8ff);
            mcu.ex_ignore = 1; mcu.pc = 0x3471; break;
        case 0x3474: mcu.pc = 0x3393; break;
        case 0x3471: case 0x348b: case 0x36a7: case 0x36ad: case 0x36da:
            ++mcu.pc; mcu.pc = MCU_PopStack(mcu); break;
        default: return false;
    }
    return true;
}

// Voice termination bookkeeping, 3393..33c9. PCM register writes stay separate.
inline bool TryUnlinkFinishedVoice(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0], index = mcu.r[1];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || mcu.pc != 0x3393
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700 || index >= 24
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    const unsigned first = MCU_Read(mcu,0xcac4+index), second = MCU_Read(mcu,0xcadc+index);
    const unsigned partner = first != 255 ? first : second;
    if (partner != 255 && partner >= 24) return false;
    MCU_Write(mcu,0xac42+index,0); MCU_Write16(mcu,voice,22);
    unsigned count = 6;
    if (partner != 255) {
        mcu.r[3] = uint16_t(partner);
        MCU_Write(mcu,0xcac4+index,255); MCU_Write(mcu,0xcadc+index,255);
        MCU_Write(mcu,0xcac4+partner,255); MCU_Write(mcu,0xcadc+partner,255);
        count = first != 255 ? 10 : 13;
        mcu.sr = uint16_t((mcu.sr&~15)|STATUS_N);
    } else mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z);
    mcu.pc = 0x33c9; mcu.native_debt = count-1;
    return true;
}

// PCM stop and task notification. Each bus write/IRQ boundary stays separate.
inline bool TryStepVoiceStop(mcu_t& mcu)
{
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~(STATUS_N|STATUS_Z|STATUS_V))
            |(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x33c9:
            if (mcu.br != 0xe0 || mcu.r[1] >= 24) return false;
            mcu.pc = 0x33cb; MCU_Write(mcu,0xe03e,uint8_t(mcu.r[1])); nz(mcu.r[1],true); break;
        case 0x33cb:
            if (mcu.br != 0xe0) return false;
            mcu.pc = 0x33d0; MCU_Write16(mcu,0xe018,0x00b6); nz(0x00b6); break;
        case 0x33d0:
            mcu.pc = 0x33d4; MCU_Write16(mcu,0xa1d4,mcu.r[1]); nz(mcu.r[1]); break;
        case 0x33d4:
            MCU_ControlRegisterWrite(mcu,0,MCU_Operand_Size::WORD,mcu.sr&0xf8ff);
            mcu.ex_ignore = 1; mcu.pc = 0x33d8; break;
        case 0x33d8: mcu.r[0] = uint16_t((mcu.r[0]&0xff00)|1); nz(1,true); mcu.pc = 0x33da; break;
        case 0x33da: mcu.r[1] = uint16_t((mcu.r[1]&0xff00)|1); nz(1,true); mcu.pc = 0x33dc; break;
        case 0x33dc: mcu.pc = 0x33de; MCU_Interrupt_TRAPA(mcu,2); break;
        default: return false;
    }
    return true;
}

// Five LFO/EG service slots with interrupt windows, 32f0..3362.
inline bool TryStepVoiceService(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a
        || mcu.pc < 0x32f0 || mcu.pc > 0x3362)
        return false;
    auto unmask = [&] {
        MCU_ControlRegisterWrite(mcu,0,MCU_Operand_Size::WORD,mcu.sr&0xf8ff);
        mcu.ex_ignore = 1; mcu.pc += 4;
    };
    if (mcu.pc == 0x335e) { unmask(); return true; }
    if (mcu.pc == 0x3362) { ++mcu.pc; mcu.pc = MCU_PopStack(mcu); return true; }
    const unsigned slot = (mcu.pc-0x32f0)/22;
    if (slot >= 5) return false;
    switch ((mcu.pc-0x32f0)%22) {
        case 0: unmask(); break;
        case 4: case 5: case 6: case 7: ++mcu.pc; break; // NOP windows cannot be removed.
        case 8:
            MCU_ControlRegisterWrite(mcu,0,MCU_Operand_Size::WORD,mcu.sr|0x0700);
            mcu.ex_ignore = 1; mcu.pc += 4; break;
        case 12: {
            const unsigned stage = ReadWord(mcu,voice);
            const uint16_t difference = uint16_t(stage-14);
            const int signedDifference = int(int16_t(stage))-14;
            mcu.sr = uint16_t((mcu.sr&~15)|(difference&0x8000 ? STATUS_N : 0)
                |(!difference ? STATUS_Z : 0)|(stage < 14 ? STATUS_C : 0)
                |(signedDifference < -32768 ? STATUS_V : 0));
            mcu.pc += 5; break;
        }
        case 17: mcu.pc = (mcu.sr&STATUS_C) ? uint16_t(mcu.pc+2) : 0x3363; break;
        case 19: {
            constexpr uint16_t targets[]{0x3a7a,0x33f4,0x4443,0x4fdb,0x36db};
            mcu.pc += 3; MCU_PushStack(mcu,mcu.pc); mcu.pc = targets[slot]; break;
        }
        default: return false;
    }
    return true;
}

// Consume the per-voice release request and prepare all three EGs, 3212..32f0.
inline bool TryPrepareVoiceRelease(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || mcu.pc != 0x3212
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    const unsigned index = ReadWord(mcu,voice-2);
    if (index >= 24) return false;
    mcu.r[1] = uint16_t(index);
    auto finish = [&](unsigned count, uint16_t flags, uint16_t pc = 0x32f0) {
        mcu.sr = uint16_t((mcu.sr&~15)|flags); mcu.pc = pc; mcu.native_debt = count-1;
    };
    if (!MCU_Read(mcu,0xac2a+index)) { finish(3,STATUS_Z); return true; }
    MCU_Write(mcu,0xac2a+index,0);
    const unsigned stage = ReadWord(mcu,voice);
    if (stage >= 12) {
        const uint16_t diff = uint16_t(stage-12);
        finish(6,uint16_t((diff&0x8000 ? STATUS_N : 0)|(!diff ? STATUS_Z : 0)
            |(int(int16_t(stage))-12 < -32768 ? STATUS_V : 0)));
        return true;
    }
    if (!stage) {
        const bool cancel = ReadWord(mcu,voice+16) != 0;
        for (unsigned offset : {0u,2u,4u}) MCU_Write16(mcu,voice+offset,cancel ? 22 : 2);
        if (cancel) MCU_Write(mcu,0xac42+index,0);
        finish(cancel ? 16 : 14,cancel ? STATUS_Z : 0,cancel ? 0x3363 : 0x32f0);
        return true;
    }
    for (unsigned offset : {0u,2u,4u}) MCU_Write16(mcu,voice+offset,12);
    MCU_Write(mcu,voice+96,uint8_t(ReadWord(mcu,voice+28)>>8));
    MCU_Write(mcu,voice+97,0); MCU_Write16(mcu,voice+8,0); MCU_Write16(mcu,voice+18,0);
    MCU_Write(mcu,voice+79,MCU_Read(mcu,voice+83)); MCU_Write(mcu,voice-8,MCU_Read(mcu,voice-4));
    MCU_Write16(mcu,voice+10,0); MCU_Write16(mcu,voice+20,0);
    MCU_Write(mcu,voice+74,MCU_Read(mcu,voice+78));
    MCU_Write16(mcu,voice+84,ReadWord(mcu,voice+32)); MCU_Write16(mcu,voice+86,ReadWord(mcu,voice+94));
    const unsigned currentHigh = MCU_Read(mcu,voice+44), targetHigh = MCU_Read(mcu,voice+111);
    const unsigned currentLow = ReadWord(mcu,voice+68), targetLow = ReadWord(mcu,voice+122);
    MCU_Write(mcu,voice+106,uint8_t(currentHigh)); MCU_Write16(mcu,voice+112,uint16_t(currentLow));
    MCU_Write(mcu,voice+107,uint8_t(targetHigh)); MCU_Write16(mcu,voice+114,uint16_t(targetLow));
    const bool falling = targetHigh < currentHigh || (targetHigh == currentHigh && targetLow < currentLow);
    MCU_Write(mcu,voice-3,falling ? 2 : 0);
    mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|targetHigh); mcu.r[5] = ReadWord(mcu,voice+132);
    MCU_Write16(mcu,voice+124,mcu.r[5]); MCU_Write16(mcu,voice+12,0); MCU_Write16(mcu,voice+22,0);
    unsigned count = 48+(falling ? 2 : 1)+(targetHigh == currentHigh ? 1 : 0);
    if (!ReadWord(mcu,voice-78)) { MCU_Write16(mcu,voice-78,0xffff); ++count; }
    auto last = ReadWord(mcu,voice-112);
    if (!last) { last = 0xffff; MCU_Write16(mcu,voice-112,last); ++count; }
    finish(count,last&0x8000 ? STATUS_N : 0);
    return true;
}

// Filter EG stage dispatch and attack/decay target setup, 4443..4494.
inline bool TryDispatchFilterStage(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || mcu.pc != 0x4443
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    const unsigned disabled = MCU_Read(mcu,voice+101);
    if (disabled) {
        mcu.sr = uint16_t((mcu.sr&~15)|(disabled&128 ? STATUS_N : 0));
        mcu.pc = 0x4448; mcu.native_debt = 1; return true;
    }
    unsigned stage = ReadWord(mcu,voice+2);
    if (stage > 22 || (stage&1)) return false;
    const bool advance = ReadWord(mcu,voice+10) == 0xffff;
    unsigned setup = 0, count = 8;
    if (advance) {
        stage = ReadWord(mcu,0x6ac8+stage);
        if (stage > 22 || (stage&1)) return false;
        setup = ReadWord(mcu,0x7896+stage);
        if (setup != 0x4469 && setup != 0x4471 && setup != 0x4479
            && setup != 0x448b && setup != 0x4553) return false;
        count = 12;
    }
    const unsigned target = setup == 0x4553 ? setup : ReadWord(mcu,0x78ae + stage);
    mcu.r[1] = ReadWord(mcu,voice-2);
    if (advance) {
        MCU_Write16(mcu,voice+10,0); MCU_Write16(mcu,voice+2,uint16_t(stage));
        if (setup == 0x4469 || setup == 0x4471 || setup == 0x4479) {
            const unsigned index = setup == 0x4469 ? 0 : setup == 0x4471 ? 1 : 2;
            mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|MCU_Read(mcu,voice+75+index));
            mcu.r[6] = ReadWord(mcu,voice+88+2*index);
            mcu.r[4] = ReadWord(mcu,voice+86);
            MCU_Write16(mcu,voice+84,mcu.r[4]); MCU_Write(mcu,voice+74,uint8_t(mcu.r[5]));
            MCU_Write16(mcu,voice+86,mcu.r[6]);
            count += index == 2 ? 9 : 10;
        } else if (setup == 0x448b) count += 3;
    }
    mcu.r[3] = uint16_t(target);
    mcu.sr = uint16_t((mcu.sr&~15)|(advance ? 0 : STATUS_C));
    mcu.pc = uint16_t(target); mcu.native_debt = count-1;
    return true;
}

// Filter EG attack/decay/release duration, including optional attack controller.
inline bool TryComputeFilterDuration(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    const bool attack = mcu.pc == 0x44a3, decay = mcu.pc == 0x44ff, release = mcu.pc == 0x4564;
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || !(attack||decay||release)
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a)
        return false;
    const bool adjust = !attack || (MCU_Read(mcu,voice+162)&16);
    const unsigned source = ReadWord(mcu,voice+46), offset = attack ? 20 : decay ? 21 : 22;
    if (adjust && (source < 0x8000 || source+offset >= 0xe000)) return false;
    const unsigned control = adjust ? MCU_Read(mcu,source+offset) : 64, base = MCU_Read(mcu,voice+74);
    unsigned adjusted, count = attack ? (adjust ? 8 : 4) : 6;
    if (!adjust) adjusted = base;
    else if (control < 64) {
        const unsigned subtract = 2*(64-control);
        adjusted = uint8_t(base-subtract); count += 4;
        if (base < subtract) { adjusted = 0; count += 2; }
    } else {
        adjusted = uint8_t(base+uint8_t(2*(control-64))); count += 3;
        if (adjusted&128) { adjusted = 127; ++count; }
    }
    if (adjusted >= 128) return false;
    const unsigned scale1 = ReadWord(mcu,voice-(release ? 42 : 44));
    const unsigned scale2 = ReadWord(mcu,voice-(attack ? 40 : 38));
    const uint32_t first = uint32_t(ReadWord(mcu,0x6f12+2*adjusted))*scale1;
    uint16_t value;
    count += 5; // index, table load, MUL, CMP, BCS
    if ((first>>16) >= 255) { value = 0xffff; count += 2; }
    else { value = uint16_t(first>>8); count += 3; }
    const uint32_t second = uint32_t(value)*scale2;
    mcu.r[2] = uint16_t(second>>16); mcu.r[3] = uint16_t(second); count += 3;
    const bool saturated = mcu.r[2] >= 255;
    if (saturated) { mcu.r[6] = 0xffff; count += 2; }
    else {
        mcu.r[3] = uint16_t(second>>8); mcu.r[2] = mcu.r[3]; mcu.r[6] = mcu.r[2];
        count += release ? 4 : 5;
    }
    mcu.sr = uint16_t((mcu.sr&~15)|(mcu.r[6]&0x8000 ? STATUS_N : 0)
        |(!mcu.r[6] ? STATUS_Z : 0)|(!saturated ? STATUS_C : 0));
    mcu.pc = 0x45b6; mcu.native_debt = count-1;
    return true;
}

// Filter EG phase and signed interpolation, 45b6..4662.
inline bool TryAdvanceFilterPhase(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0], duration = mcu.r[6];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || mcu.pc != 0x45b6
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](uint16_t value, bool carry = false, bool overflow = false) {
        mcu.sr = uint16_t((mcu.sr&~15)|(value&0x8000 ? STATUS_N : 0)
            |(!value ? STATUS_Z : 0)|(carry ? STATUS_C : 0)|(overflow ? STATUS_V : 0));
    };
    auto arithmetic = [&](uint16_t a, uint16_t b, bool subtract) {
        const uint16_t value = uint16_t(subtract ? a-b : a+b);
        const int signedValue = subtract ? int(int16_t(a))-int(int16_t(b))
                                        : int(int16_t(a))+int(int16_t(b));
        nz(value,subtract ? a < b : unsigned(a)+b > 65535,
           signedValue < -32768 || signedValue > 32767);
        return value;
    };
    auto multiply = [&] {
        const uint32_t product = uint32_t(mcu.r[2])*mcu.r[3];
        mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
    };
    auto finish = [&](unsigned count) {
        MCU_Write16(mcu,voice+32,mcu.r[5]);
        nz(mcu.r[5],(mcu.sr&STATUS_C) != 0); // MOV clears V, preserves C
        mcu.pc = 0x4662; mcu.native_debt = count; // final store adds one
    };
    const uint16_t target = ReadWord(mcu,voice+86);
    if (duration <= 8) {
        MCU_Write16(mcu,voice+10,0xffff); MCU_Write16(mcu,voice-48,uint16_t(duration));
        mcu.r[5] = target; nz(target,duration < 8); finish(5); return true;
    }
    const unsigned rate = 0x80000/duration;
    const unsigned elapsed = uint16_t(ReadWord(mcu,0xac5a)+ReadWord(mcu,voice+20));
    const uint32_t phase = elapsed*rate+ReadWord(mcu,voice+10);
    MCU_Write16(mcu,voice-48,8); MCU_Write16(mcu,voice+20,0);
    mcu.r[1] = uint16_t(rate); mcu.r[2] = uint16_t(phase>>16); mcu.r[3] = uint16_t(phase);
    if (mcu.r[2]) {
        const uint32_t excess = phase-65535;
        mcu.r[2] = uint16_t(excess%rate); mcu.r[3] = uint16_t(excess/rate);
        MCU_Write16(mcu,voice+20,mcu.r[3]); MCU_Write16(mcu,voice+10,0xffff);
        mcu.r[5] = target; nz(target); finish(23); return true;
    }
    MCU_Write16(mcu,voice+10,mcu.r[3]);
    mcu.r[5] = ReadWord(mcu,voice+84); mcu.r[2] = target;
    const bool startNegative = (mcu.r[5]&0x8000) != 0, targetNegative = (target&0x8000) != 0;
    unsigned count;
    if (startNegative != targetNegative) {
        if (startNegative) {
            mcu.r[2] = uint16_t(target-mcu.r[5]); count = 10;
        } else {
            mcu.r[2] = uint16_t(uint16_t(0-target)+mcu.r[5]); count = 11;
        }
        if (mcu.r[2]&0x8000) { mcu.r[2] = 0x7fff; ++count; }
        multiply(); mcu.r[5] = arithmetic(mcu.r[5],mcu.r[2],!startNegative);
    } else {
        if (startNegative) { mcu.r[2] = uint16_t(0-mcu.r[2]); mcu.r[5] = uint16_t(0-mcu.r[5]); }
        const bool falling = mcu.r[2] < mcu.r[5];
        mcu.r[2] = uint16_t(mcu.r[2]-mcu.r[5]);
        if (falling) mcu.r[2] = uint16_t(0-mcu.r[2]);
        multiply(); mcu.r[5] = arithmetic(mcu.r[5],mcu.r[2],falling);
        if (startNegative) mcu.r[5] = arithmetic(0,mcu.r[5],true);
        count = (startNegative ? 13 : 11)+(falling ? 1 : 0);
    }
    finish(16+count);
    return true;
}

// Filter base/controller and signed offset, 4662..46c3, before LFO calls.
inline bool TryAdjustFilterBase(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || mcu.pc != 0x4662
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    const unsigned source = ReadWord(mcu,voice+46);
    if (source < 0x8000 || source+18 >= 0xe000) return false;
    auto mov = [&](uint16_t value) {
        mcu.sr = uint16_t((mcu.sr&~(STATUS_N|STATUS_Z|STATUS_V))
            |(value&0x8000 ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto add = [&](uint16_t a, uint16_t b, bool subtract = false) {
        const uint16_t value = uint16_t(subtract ? a-b : a+b);
        const int signedValue = subtract ? int(int16_t(a))-int(int16_t(b))
                                        : int(int16_t(a))+int(int16_t(b));
        mcu.sr = uint16_t((mcu.sr&~15)|(value&0x8000 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
            |((subtract ? a < b : unsigned(a)+b > 65535) ? STATUS_C : 0)
            |(signedValue < -32768 || signedValue > 32767 ? STATUS_V : 0));
        return value;
    };
    mcu.r[1] = ReadWord(mcu,voice-2);
    unsigned base = MCU_Read(mcu,voice+163), control = MCU_Read(mcu,source+18);
    unsigned delta = uint8_t(control-64), count = 7;
    if (control < 64) {
        delta = uint8_t(0-delta); base = uint8_t(base-delta); count += 3;
        if (base&128) { base = 0; count += 2; }
    } else {
        count += 2;
        if (!(MCU_Read(mcu,voice+162)&4)) {
            count += 4;
            if (delta >= 16) { delta = 16; ++count; }
            base = uint8_t(base+delta);
            if (base&128) { base = 127; ++count; }
        }
    }
    mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|delta); mcu.r[4] = uint16_t(base<<8);
    const bool negative = (mcu.r[5]&0x8000) != 0;
    mcu.r[5] = add(mcu.r[5],mcu.r[4]); count += 5;
    if (mcu.r[5]&0x8000) {
        if (negative) { mcu.r[5] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); ++count; }
        else { mcu.r[5] = 0x7fff; mov(mcu.r[5]); count += 2; }
    }
    MCU_Write16(mcu,voice+34,mcu.r[5]); mov(mcu.r[5]); ++count;
    mcu.r[2] = ReadWord(mcu,voice+136); mov(mcu.r[2]); count += 2;
    if (mcu.r[2]) {
        ++count;
        if (mcu.r[2]&0x8000) {
            mcu.r[2] = uint16_t(0-mcu.r[2]);
            mcu.r[5] = add(mcu.r[5],mcu.r[2],true); count += 3;
            if (mcu.sr&STATUS_C) {
                mcu.r[5] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); count += 2;
            }
        } else {
            mcu.r[5] = add(mcu.r[5],mcu.r[2]); count += 2;
            if (mcu.sr&STATUS_V) { mcu.r[5] = 0x7fff; mov(mcu.r[5]); ++count; }
        }
    }
    mcu.pc = 0x46c3; mcu.native_debt = count-1;
    return true;
}

// Signed filter LFO depth composition and rounded modulation, 47fb..4856.
inline bool TryModulateFilter(mcu_t& mcu)
{
    if (!mcu.native_v121_enabled || mcu.cp || mcu.pc != 0x47fb
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700) return false;
    const bool aNegative = (mcu.r[2]&0x8000) != 0, bNegative = (mcu.r[3]&0x8000) != 0;
    bool negative = false;
    unsigned count = 4;
    if (!aNegative && !bNegative) {
        mcu.r[2] = uint16_t(mcu.r[2]+mcu.r[3]); count += 3;
        if (mcu.r[2] > 0x1800) { mcu.r[2] = 0x1800; count += 2; }
    } else if (aNegative && bNegative) {
        // The ROM negates both registers but does not add them on this branch.
        mcu.r[2] = uint16_t(0-mcu.r[2]); mcu.r[3] = uint16_t(0-mcu.r[3]);
        negative = true; count += 4;
        if (mcu.r[2] > 0x1800) { mcu.r[2] = 0x1800; count += 2; }
    } else {
        mcu.r[2] = uint16_t(mcu.r[2]+mcu.r[3]); count += 2;
        if (mcu.r[2]&0x8000) { mcu.r[2] = uint16_t(0-mcu.r[2]); negative = true; count += 2; }
    }
    const bool waveNegative = (mcu.r[6]&0x8000) != 0;
    count += 2;
    if (waveNegative) { mcu.r[6] = uint16_t(0-mcu.r[6]); count += negative ? 1 : 2; }
    const bool subtract = negative != waveNegative;
    mcu.r[6] = uint16_t(mcu.r[6]*2);
    const uint32_t product = uint32_t(mcu.r[2])*mcu.r[6];
    mcu.r[2] = uint16_t(product>>16);
    mcu.r[3] = uint16_t(product+0x8000);
    const unsigned carry = (product&0xffff) >= 0x8000 ? 1 : 0;
    const unsigned previous = mcu.r[5], delta = unsigned(mcu.r[2])+carry;
    const int signedValue = subtract ? int(int16_t(previous))-int(int16_t(mcu.r[2]))-int(carry)
                                    : int(int16_t(previous))+int(int16_t(mcu.r[2]))+int(carry);
    mcu.r[5] = uint16_t(subtract ? previous-delta : previous+delta);
    const bool overflow = signedValue < -32768 || signedValue > 32767;
    const bool carryOut = subtract ? previous < delta : previous+delta > 65535;
    mcu.sr = uint16_t((mcu.sr&~15)|(mcu.r[5]&0x8000 ? STATUS_N : 0)
        |(!mcu.r[5] && (subtract || !mcu.r[3]) ? STATUS_Z : 0)
        |(overflow ? STATUS_V : 0)|(carryOut ? STATUS_C : 0));
    count += 5;
    if (subtract && carryOut) {
        mcu.r[5] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); ++count;
    } else if (!subtract && overflow) {
        mcu.r[5] = 0x7fff; mcu.sr = uint16_t(mcu.sr&~(STATUS_N|STATUS_Z|STATUS_V)); count += 2;
    }
    mcu.pc = 0x4856; mcu.native_debt = count-1;
    return true;
}

// Resonance controller target, one-step smoothing and cutoff limit, 46dd..473c.
inline bool TryAdjustFilterResonance(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || mcu.pc != 0x46dd
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    const unsigned source = ReadWord(mcu,voice+46);
    if (source < 0x8000 || source+19 >= 0xe000) return false;
    const unsigned maximum = MCU_Read(mcu,voice+105), previous = MCU_Read(mcu,voice+104);
    unsigned target = MCU_Read(mcu,voice+103), delta = uint8_t(MCU_Read(mcu,source+19)-64), count = 7;
    if (!(delta&128)) {
        delta = uint8_t(delta*2); target = uint8_t(target-delta); count += 3;
        if (target&128) { target = 0; ++count; }
        else { count += 2; if (target >= maximum) { target = maximum; count += 2; } }
    } else {
        delta = uint8_t(uint8_t(0-delta)*2); target = uint8_t(target+delta); count += 5;
        if (target >= maximum) { target = maximum; count += 2; }
    }
    MCU_Write16(mcu,voice+34,mcu.r[5]);
    mcu.r[2] = uint16_t(source);
    mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|target);
    mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|maximum);
    auto compare = [&](unsigned a, unsigned b) {
        const uint8_t difference = uint8_t(a-b);
        const int signedDifference = int(int8_t(a))-int(int8_t(b));
        mcu.sr = uint16_t((mcu.sr&~15)|(difference&128 ? STATUS_N : 0)
            |(!difference ? STATUS_Z : 0)|(a < b ? STATUS_C : 0)
            |(signedDifference < -128 || signedDifference > 127 ? STATUS_V : 0));
    };
    mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|previous); count += 3;
    compare(target,previous);
    if (target != previous) {
        unsigned value = uint8_t(target < previous ? previous-1 : previous+1);
        count += target < previous ? 3 : 2;
        const unsigned sum = unsigned(ReadWord(mcu,voice+36))+255;
        const unsigned index = sum > 65535 ? 255 : sum>>8;
        mcu.r[3] = MCU_Read(mcu,0x7714+index); count += 8+(sum > 65535 ? 1 : 0);
        compare(mcu.r[3],value);
        if (mcu.r[3] < value) { value = mcu.r[3]; ++count; }
        mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|value);
        MCU_Write(mcu,voice+104,uint8_t(value)); ++count;
        mcu.sr = uint16_t((mcu.sr&~(STATUS_N|STATUS_Z|STATUS_V))
            |(value&128 ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    }
    mcu.pc = 0x473c; mcu.native_debt = count-1;
    return true;
}

// LFO argument loads/calls and filter returns, one original instruction per step.
inline bool TryStepFilterConnections(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    unsigned reg, address, length;
    switch (mcu.pc) {
        case 0x46c3: reg = 2; address = voice-120; length = 3; break;
        case 0x46c6: reg = 3; address = voice+140; length = 4; break;
        case 0x46ca: reg = 6; address = voice-96; length = 3; break;
        case 0x46d0: reg = 2; address = voice-86; length = 3; break;
        case 0x46d3: reg = 3; address = voice+148; length = 4; break;
        case 0x46d7: reg = 6; address = voice-62; length = 3; break;
        case 0x46cd: case 0x46da:
            mcu.pc += 3; MCU_PushStack(mcu,mcu.pc); mcu.pc = 0x47fb; return true;
        case 0x4448: case 0x47ee: case 0x47f4: case 0x47fa: case 0x4856:
            ++mcu.pc; mcu.pc = MCU_PopStack(mcu); return true;
        default: return false;
    }
    mcu.pc = uint16_t(mcu.pc+length);
    mcu.r[reg] = ReadWord(mcu,address);
    mcu.sr = uint16_t((mcu.sr&~(STATUS_N|STATUS_Z|STATUS_V))
        |(mcu.r[reg]&0x8000 ? STATUS_N : 0)|(!mcu.r[reg] ? STATUS_Z : 0));
    return true;
}

// Filter initial output and held target, 4494/4553..4662.
inline bool TrySetFilterImmediate(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    const bool initial = mcu.pc == 0x4494;
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || mcu.ep || (!initial && mcu.pc != 0x4553)
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    if (initial) { MCU_Write16(mcu,voice+36,0); MCU_Write16(mcu,voice-48,0); }
    else MCU_Write16(mcu,voice+20,0);
    mcu.r[5] = ReadWord(mcu,voice+(initial ? 84 : 86));
    MCU_Write16(mcu,voice+32,mcu.r[5]);
    if (!initial) MCU_Write16(mcu,voice-48,8);
    mcu.sr = uint16_t((mcu.sr&~15)|(initial
        ? ((mcu.r[5]&0x8000 ? STATUS_N : 0)|(!mcu.r[5] ? STATUS_Z : 0)) : 0));
    mcu.pc = 0x4662; mcu.native_debt = 4;
    return true;
}

// Interruptible form of 47fb..4854. No debt: publish every instruction result.
inline bool TryStepFilterModulation(mcu_t& mcu)
{
    if (!mcu.native_v121_enabled || mcu.cp || (mcu.sr&STATUS_T)) return false;
    auto arithmetic = [&](uint16_t a, uint16_t b, bool sub = false, unsigned carry = 0) {
        const uint16_t value = uint16_t(sub ? a-b-carry : a+b+carry);
        const int signedValue = sub ? int(int16_t(a))-int(int16_t(b))-int(carry)
                                    : int(int16_t(a))+int(int16_t(b))+int(carry);
        mcu.sr = uint16_t((mcu.sr&~15)|(value&0x8000 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
            |((sub ? unsigned(a) < unsigned(b)+carry : unsigned(a)+b+carry > 65535) ? STATUS_C : 0)
            |(signedValue < -32768 || signedValue > 32767 ? STATUS_V : 0));
        return value;
    };
    auto test = [&](unsigned reg) { const auto value = mcu.r[reg];
        mcu.sr = uint16_t((mcu.sr&~15)|(value&0x8000 ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto branch = [&](bool take, uint16_t target) { mcu.pc = take ? target : uint16_t(mcu.pc+2); };
    switch (mcu.pc) {
        case 0x47fb: test(2); mcu.pc += 2; break;
        case 0x47ff: case 0x480f: test(3); mcu.pc += 2; break;
        case 0x4829: case 0x4831: test(6); mcu.pc += 2; break;
        case 0x47fd: branch(mcu.sr&STATUS_N,0x480f); break;
        case 0x4801: branch(mcu.sr&STATUS_N,0x4821); break;
        case 0x4811: branch(!(mcu.sr&STATUS_N),0x4821); break;
        case 0x4823: branch(!(mcu.sr&STATUS_N),0x4829); break;
        case 0x482b: branch(!(mcu.sr&STATUS_N),0x4837); break;
        case 0x4833: branch(!(mcu.sr&STATUS_N),0x4848); break;
        case 0x4808: branch(mcu.sr&(STATUS_C|STATUS_Z),0x4829); break;
        case 0x481a: branch(mcu.sr&(STATUS_C|STATUS_Z),0x4831); break;
        case 0x4841: branch(!(mcu.sr&STATUS_V),0x4856); break;
        case 0x4852: branch(!(mcu.sr&STATUS_C),0x4856); break;
        case 0x480d: mcu.pc = 0x4829; break;
        case 0x481f: case 0x4827: mcu.pc = 0x4831; break;
        case 0x482f: mcu.pc = 0x4848; break;
        case 0x4846: mcu.pc = 0x4856; break;
        case 0x4803: case 0x4821: mcu.r[2] = arithmetic(mcu.r[2],mcu.r[3]); mcu.pc += 2; break;
        case 0x4805: case 0x4817: arithmetic(mcu.r[2],0x1800,true); mcu.pc += 3; break;
        case 0x480a: case 0x481c:
            mcu.r[2] = 0x1800; mcu.sr &= ~(STATUS_N|STATUS_Z|STATUS_V); mcu.pc += 3; break;
        case 0x4813: case 0x4825: mcu.r[2] = arithmetic(0,mcu.r[2],true); mcu.pc += 2; break;
        case 0x4815: mcu.r[3] = arithmetic(0,mcu.r[3],true); mcu.pc += 2; break;
        case 0x482d: case 0x4835: mcu.r[6] = arithmetic(0,mcu.r[6],true); mcu.pc += 2; break;
        case 0x4837: case 0x4848: mcu.r[6] = arithmetic(mcu.r[6],mcu.r[6]); mcu.pc += 2; break;
        case 0x4839: case 0x484a: {
            const uint32_t product = uint32_t(mcu.r[2])*mcu.r[6];
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += 2; break;
        }
        case 0x483b: case 0x484c: mcu.r[3] = arithmetic(mcu.r[3],0x8000); mcu.pc += 4; break;
        case 0x483f: case 0x4850: {
            const bool sub = mcu.pc == 0x4850, zero = (mcu.sr&STATUS_Z) != 0;
            mcu.r[5] = arithmetic(mcu.r[5],mcu.r[2],sub,(mcu.sr&STATUS_C) ? 1 : 0);
            if (!sub && !zero) mcu.sr &= ~STATUS_Z;
            mcu.pc += 2; break;
        }
        case 0x4843: mcu.r[5] = 0x7fff; mcu.sr &= ~(STATUS_N|STATUS_Z|STATUS_V); mcu.pc += 3; break;
        case 0x4854: mcu.r[5] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        default: return false;
    }
    return true;
}

// Interruptible filter stages, duration, phase and cutoff encoding.
inline bool TryStepFilterConversion(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~(STATUS_N|STATUS_Z|STATUS_V))
            |(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto arithmetic = [&](unsigned a, unsigned b, bool sub = false, bool byte = false) {
        const unsigned mask = byte ? 255 : 65535; a &= mask; b &= mask;
        const unsigned value = (sub ? a-b : a+b)&mask;
        const int sa = byte ? int(int8_t(a)) : int(int16_t(a));
        const int sb = byte ? int(int8_t(b)) : int(int16_t(b));
        const int result = sub ? sa-sb : sa+sb, limit = byte ? 128 : 32768;
        nz(value,byte); mcu.sr &= ~STATUS_C;
        if (sub ? a < b : a+b > mask) mcu.sr |= STATUS_C;
        if (result < -limit || result >= limit) mcu.sr |= STATUS_V;
        return uint16_t(value);
    };
    auto branch = [&](bool take, uint16_t target) { mcu.pc = take ? target : uint16_t(mcu.pc+2); };
    switch (mcu.pc) {
        case 0x46dd: MCU_Write16(mcu,voice+34,mcu.r[5]); nz(mcu.r[5]); mcu.pc += 3; break;
        case 0x4662: mcu.r[1] = ReadWord(mcu,voice-2); nz(mcu.r[1]); mcu.pc += 3; break;
        case 0x45b6: arithmetic(mcu.r[6],8,true); mcu.pc += 3; break;
        case 0x4443: nz(MCU_Read(mcu,voice+101),true); mcu.sr &= ~STATUS_C; mcu.pc += 3; break;
        case 0x4446: branch(mcu.sr&STATUS_Z,0x4449); break;
        case 0x4449: mcu.r[1] = ReadWord(mcu,voice-2); nz(mcu.r[1]); mcu.pc += 3; break;
        case 0x444c: case 0x4459: case 0x448b: mcu.r[3] = ReadWord(mcu,voice+2); nz(mcu.r[3]); mcu.pc += 3; break;
        case 0x444f: arithmetic(ReadWord(mcu,voice+10),0xffff,true); mcu.pc += 5; break;
        case 0x4454: branch(!(mcu.sr&STATUS_Z),0x448e); break;
        case 0x4456: MCU_Write16(mcu,voice+10,0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 3; break;
        case 0x445c: case 0x4463: case 0x448e: {
            if (mcu.r[3] > 22 || (mcu.r[3]&1)) return false;
            const unsigned table = mcu.pc == 0x445c ? 0x6ac8 : mcu.pc == 0x4463 ? 0x7896 : 0x78ae;
            mcu.r[3] = ReadWord(mcu,table+mcu.r[3]); nz(mcu.r[3]); mcu.pc += 4; break;
        }
        case 0x4460: MCU_Write16(mcu,voice+2,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; break;
        case 0x4467: case 0x4492: mcu.pc = mcu.r[3]; break;
        case 0x4469: case 0x4471: case 0x4479: {
            const unsigned offset = mcu.pc == 0x4469 ? 75 : mcu.pc == 0x4471 ? 76 : 77;
            mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|MCU_Read(mcu,voice+offset)); nz(mcu.r[5],true); mcu.pc += 3; break;
        }
        case 0x446c: case 0x4474: case 0x447c: {
            const unsigned offset = mcu.pc == 0x446c ? 88 : mcu.pc == 0x4474 ? 90 : 92;
            mcu.r[6] = ReadWord(mcu,voice+offset); nz(mcu.r[6]); mcu.pc += 3; break;
        }
        case 0x446f: case 0x4477: mcu.pc = 0x447f; break;
        case 0x447f: mcu.r[4] = ReadWord(mcu,voice+86); nz(mcu.r[4]); mcu.pc += 3; break;
        case 0x4482: MCU_Write16(mcu,voice+84,mcu.r[4]); nz(mcu.r[4]); mcu.pc += 3; break;
        case 0x4485: MCU_Write(mcu,voice+74,uint8_t(mcu.r[5])); nz(mcu.r[5],true); mcu.pc += 3; break;
        case 0x4488: MCU_Write16(mcu,voice+86,mcu.r[6]); nz(mcu.r[6]); mcu.pc += 3; break;
        case 0x44ff: mcu.r[2] = ReadWord(mcu,voice+46); nz(mcu.r[2]); mcu.pc += 3; break;
        case 0x4502: if (mcu.r[2] < 0x8000 || unsigned(mcu.r[2])+(21) >= 0xe000) return false;
            mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|MCU_Read(mcu,mcu.r[2]+(21))); nz(mcu.r[2],true); mcu.pc += 3; break;
        case 0x4505: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x4507: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|MCU_Read(mcu,voice+74)); nz(mcu.r[3],true); mcu.pc += 3; break;
        case 0x450a: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|arithmetic(mcu.r[2],64,true,true)); mcu.pc += 3; break;
        case 0x450d: branch(!(mcu.sr&STATUS_C),0x451b); break;
        case 0x450f: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|arithmetic(0,mcu.r[2],true,true)); mcu.pc += 2; break;
        case 0x4511: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|arithmetic(mcu.r[2],mcu.r[2],false,true)); mcu.pc += 2; break;
        case 0x4513: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|arithmetic(mcu.r[3],mcu.r[2],true,true)); mcu.pc += 2; break;
        case 0x4515: branch(!(mcu.sr&STATUS_C),0x4523); break;
        case 0x4517: mcu.r[3] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x4519: mcu.pc = 0x4523; break;
        case 0x451b: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|arithmetic(mcu.r[2],mcu.r[2],false,true)); mcu.pc += 2; break;
        case 0x451d: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|arithmetic(mcu.r[3],mcu.r[2],false,true)); mcu.pc += 2; break;
        case 0x451f: branch(!(mcu.sr&STATUS_N),0x4523); break;
        case 0x4521: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|127); nz(mcu.r[3],true); mcu.pc += 2; break;
        case 0x4523: mcu.r[3] = arithmetic(mcu.r[3],mcu.r[3]); mcu.pc += 2; break;
        case 0x4525: if (mcu.r[3] > 254) return false;
            mcu.r[2] = ReadWord(mcu,0x6f12+mcu.r[3]); nz(mcu.r[2]); mcu.pc += 4; break;
        case 0x4529: {
            const uint32_t product = uint32_t(mcu.r[2])*ReadWord(mcu,voice-44);
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0)); mcu.pc += 3; break;
        }
        case 0x452c: arithmetic(mcu.r[2],255,true); mcu.pc += 3; break;
        case 0x452f: branch(mcu.sr&STATUS_C,0x4536); break;
        case 0x4531: mcu.r[2] = 0xffff; nz(mcu.r[2]); mcu.pc += 3; break;
        case 0x4534: mcu.pc = 0x453c; break;
        case 0x4536: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|(mcu.r[2]&255)); nz(mcu.r[3],true); mcu.pc += 2; break;
        case 0x4538: mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; break;
        case 0x453a: mcu.r[2] = mcu.r[3]; nz(mcu.r[2]); mcu.pc += 2; break;
        case 0x453c: {
            const uint32_t product = uint32_t(mcu.r[2])*ReadWord(mcu,voice-38);
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0)); mcu.pc += 3; break;
        }
        case 0x453f: arithmetic(mcu.r[2],255,true); mcu.pc += 3; break;
        case 0x4542: branch(mcu.sr&STATUS_C,0x4549); break;
        case 0x4544: mcu.r[6] = 0xffff; nz(mcu.r[6]); mcu.pc += 3; break;
        case 0x4547: mcu.pc = 0x45b6; break;
        case 0x4549: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|(mcu.r[2]&255)); nz(mcu.r[3],true); mcu.pc += 2; break;
        case 0x454b: mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; break;
        case 0x454d: mcu.r[2] = mcu.r[3]; nz(mcu.r[2]); mcu.pc += 2; break;
        case 0x454f: mcu.r[6] = mcu.r[2]; nz(mcu.r[6]); mcu.pc += 2; break;
        case 0x4551: mcu.pc = 0x45b6; break;
        case 0x4564: mcu.r[2] = ReadWord(mcu,voice+46); nz(mcu.r[2]); mcu.pc += 3; break;
        case 0x4567: if (mcu.r[2] < 0x8000 || unsigned(mcu.r[2])+(22) >= 0xe000) return false;
            mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|MCU_Read(mcu,mcu.r[2]+(22))); nz(mcu.r[2],true); mcu.pc += 3; break;
        case 0x456a: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x456c: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|MCU_Read(mcu,voice+74)); nz(mcu.r[3],true); mcu.pc += 3; break;
        case 0x456f: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|arithmetic(mcu.r[2],64,true,true)); mcu.pc += 3; break;
        case 0x4572: branch(!(mcu.sr&STATUS_C),0x4580); break;
        case 0x4574: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|arithmetic(0,mcu.r[2],true,true)); mcu.pc += 2; break;
        case 0x4576: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|arithmetic(mcu.r[2],mcu.r[2],false,true)); mcu.pc += 2; break;
        case 0x4578: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|arithmetic(mcu.r[3],mcu.r[2],true,true)); mcu.pc += 2; break;
        case 0x457a: branch(!(mcu.sr&STATUS_C),0x4588); break;
        case 0x457c: mcu.r[3] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x457e: mcu.pc = 0x4588; break;
        case 0x4580: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|arithmetic(mcu.r[2],mcu.r[2],false,true)); mcu.pc += 2; break;
        case 0x4582: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|arithmetic(mcu.r[3],mcu.r[2],false,true)); mcu.pc += 2; break;
        case 0x4584: branch(!(mcu.sr&STATUS_N),0x4588); break;
        case 0x4586: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|127); nz(mcu.r[3],true); mcu.pc += 2; break;
        case 0x4588: mcu.r[3] = arithmetic(mcu.r[3],mcu.r[3]); mcu.pc += 2; break;
        case 0x458a: if (mcu.r[3] > 254) return false;
            mcu.r[2] = ReadWord(mcu,0x6f12+mcu.r[3]); nz(mcu.r[2]); mcu.pc += 4; break;
        case 0x458e: {
            const uint32_t product = uint32_t(mcu.r[2])*ReadWord(mcu,voice-42);
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0)); mcu.pc += 3; break;
        }
        case 0x4591: arithmetic(mcu.r[2],255,true); mcu.pc += 3; break;
        case 0x4594: branch(mcu.sr&STATUS_C,0x459b); break;
        case 0x4596: mcu.r[2] = 0xffff; nz(mcu.r[2]); mcu.pc += 3; break;
        case 0x4599: mcu.pc = 0x45a1; break;
        case 0x459b: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|(mcu.r[2]&255)); nz(mcu.r[3],true); mcu.pc += 2; break;
        case 0x459d: mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; break;
        case 0x459f: mcu.r[2] = mcu.r[3]; nz(mcu.r[2]); mcu.pc += 2; break;
        case 0x45a1: {
            const uint32_t product = uint32_t(mcu.r[2])*ReadWord(mcu,voice-38);
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0)); mcu.pc += 3; break;
        }
        case 0x45a4: arithmetic(mcu.r[2],255,true); mcu.pc += 3; break;
        case 0x45a7: branch(mcu.sr&STATUS_C,0x45ae); break;
        case 0x45a9: mcu.r[6] = 0xffff; nz(mcu.r[6]); mcu.pc += 3; break;
        case 0x45ac: mcu.pc = 0x45b6; break;
        case 0x45ae: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|(mcu.r[2]&255)); nz(mcu.r[3],true); mcu.pc += 2; break;
        case 0x45b0: mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; break;
        case 0x45b2: mcu.r[2] = mcu.r[3]; nz(mcu.r[2]); mcu.pc += 2; break;
        case 0x45b4: mcu.r[6] = mcu.r[2]; nz(mcu.r[6]); mcu.pc += 2; break;
        case 0x44a3: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x4494: MCU_Write16(mcu,voice+36,0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 3; break;
        case 0x4497: MCU_Write16(mcu,voice-48,0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 3; break;
        case 0x449a: mcu.r[5] = ReadWord(mcu,voice+84); nz(mcu.r[5]); mcu.pc += 3; break;
        case 0x449d: MCU_Write16(mcu,voice+32,mcu.r[5]); nz(mcu.r[5]); mcu.pc += 3; break;
        case 0x44a0: case 0x4561: mcu.pc = 0x4662; break;
        case 0x4553: MCU_Write16(mcu,voice+20,0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 3; break;
        case 0x4556: mcu.r[5] = ReadWord(mcu,voice+86); nz(mcu.r[5]); mcu.pc += 3; break;
        case 0x4559: MCU_Write16(mcu,voice+32,mcu.r[5]); nz(mcu.r[5]); mcu.pc += 3; break;
        case 0x455c: MCU_Write16(mcu,voice-48,8); nz(8); mcu.pc += 5; break;
        case 0x44a5: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|MCU_Read(mcu,voice+74)); nz(mcu.r[3],true); mcu.pc += 3; break;
        case 0x44a8: mcu.sr = uint16_t((mcu.sr&~STATUS_Z)|((MCU_Read(mcu,voice+162)&16) ? 0 : STATUS_Z)); mcu.pc += 4; break;
        case 0x44ac: branch(mcu.sr&STATUS_Z,0x44cd); break;
        case 0x44ae: mcu.r[2] = ReadWord(mcu,voice+46); nz(mcu.r[2]); mcu.pc += 3; break;
        case 0x44b1:
            if (mcu.r[2] < 0x8000 || unsigned(mcu.r[2])+20 >= 0xe000) return false;
            mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|MCU_Read(mcu,mcu.r[2]+20)); nz(mcu.r[2],true); mcu.pc += 3; break;
        case 0x44b4: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|arithmetic(mcu.r[2],64,true,true)); mcu.pc += 3; break;
        case 0x44b7: branch(!(mcu.sr&STATUS_C),0x44c5); break;
        case 0x44b9: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|arithmetic(0,mcu.r[2],true,true)); mcu.pc += 2; break;
        case 0x44bb: case 0x44c5: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|arithmetic(mcu.r[2],mcu.r[2],false,true)); mcu.pc += 2; break;
        case 0x44bd: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|arithmetic(mcu.r[3],mcu.r[2],true,true)); mcu.pc += 2; break;
        case 0x44bf: branch(!(mcu.sr&STATUS_C),0x44cd); break;
        case 0x44c1: mcu.r[3] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x44c3: mcu.pc = 0x44cd; break;
        case 0x44c7: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|arithmetic(mcu.r[3],mcu.r[2],false,true)); mcu.pc += 2; break;
        case 0x44c9: branch(!(mcu.sr&STATUS_N),0x44cd); break;
        case 0x44cb: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|127); nz(mcu.r[3],true); mcu.pc += 2; break;
        case 0x44cd: mcu.r[3] = arithmetic(mcu.r[3],mcu.r[3]); mcu.pc += 2; break;
        case 0x44cf:
            if (mcu.r[3] > 254) return false;
            mcu.r[2] = ReadWord(mcu,0x6f12+mcu.r[3]); nz(mcu.r[2]); mcu.pc += 4; break;
        case 0x44d3: case 0x44e6: {
            const uint32_t product = uint32_t(mcu.r[2])*ReadWord(mcu,voice-(mcu.pc == 0x44d3 ? 44 : 40));
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0)); mcu.pc += 3; break;
        }
        case 0x44d6: case 0x44e9: arithmetic(mcu.r[2],255,true); mcu.pc += 3; break;
        case 0x44d9: branch(mcu.sr&STATUS_C,0x44e0); break;
        case 0x44db: mcu.r[2] = 0xffff; nz(mcu.r[2]); mcu.pc += 3; break;
        case 0x44de: mcu.pc = 0x44e6; break;
        case 0x44e0: case 0x44f4: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|(mcu.r[2]&255)); nz(mcu.r[3],true); mcu.pc += 2; break;
        case 0x44e2: case 0x44f6: mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; break;
        case 0x44e4: case 0x44f8: mcu.r[2] = mcu.r[3]; nz(mcu.r[2]); mcu.pc += 2; break;
        case 0x44ec: branch(mcu.sr&STATUS_C,0x44f4); break;
        case 0x44ee: mcu.r[6] = 0xffff; nz(mcu.r[6]); mcu.pc += 3; break;
        case 0x44f1: case 0x44fc: mcu.pc = 0x45b6; break;
        case 0x44fa: mcu.r[6] = mcu.r[2]; nz(mcu.r[6]); mcu.pc += 2; break;
        case 0x45b9: mcu.pc = (mcu.sr&(STATUS_C|STATUS_Z)) ? 0x4654 : 0x45bc; break;
        case 0x45bc: mcu.r[2] = 8; nz(mcu.r[2]); mcu.pc += 3; break;
        case 0x45bf: MCU_Write16(mcu,voice-48,mcu.r[2]); nz(mcu.r[2]); mcu.pc += 3; break;
        case 0x45c2: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x45c4: case 0x45e9: {
            const unsigned divisor = mcu.r[mcu.pc == 0x45c4 ? 6 : 1];
            if (!divisor) return false;
            const uint32_t dividend = (uint32_t(mcu.r[2])<<16)|mcu.r[3];
            const uint32_t quotient = dividend/divisor;
            mcu.sr &= ~15;
            if (quotient > 65535) mcu.sr |= STATUS_V;
            else { mcu.r[2] = uint16_t(dividend%divisor); mcu.r[3] = uint16_t(quotient); nz(mcu.r[3]); }
            mcu.pc += 2; break;
        }
        case 0x45c6: mcu.r[2] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x45c8: mcu.r[2] = ReadWord(mcu,0xac5a); nz(mcu.r[2]); mcu.pc += 4; break;
        case 0x45cc: mcu.r[2] = arithmetic(mcu.r[2],ReadWord(mcu,voice+20)); mcu.pc += 3; break;
        case 0x45cf: MCU_Write16(mcu,voice+20,0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 3; break;
        case 0x45d2: mcu.r[1] = mcu.r[3]; nz(mcu.r[1]); mcu.pc += 2; break;
        case 0x45d4: case 0x4613: case 0x4622: case 0x462e: case 0x4634: case 0x4644: case 0x464c: {
            const uint32_t product = uint32_t(mcu.r[2])*mcu.r[3];
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0)); mcu.pc += 2; break;
        }
        case 0x45d6: mcu.r[3] = arithmetic(mcu.r[3],ReadWord(mcu,voice+10)); mcu.pc += 3; break;
        case 0x45d9: case 0x45e5: {
            const bool subtract = mcu.pc == 0x45e5, zero = (mcu.sr&STATUS_Z) != 0;
            mcu.r[2] = arithmetic(mcu.r[2],(mcu.sr&STATUS_C) ? 1 : 0,subtract);
            if (!subtract && !zero) mcu.sr &= ~STATUS_Z;
            mcu.pc += 4; break;
        }
        case 0x45dd: nz(mcu.r[2]); mcu.sr &= ~STATUS_C; mcu.pc += 2; break;
        case 0x45df: branch(mcu.sr&STATUS_Z,0x45f8); break;
        case 0x45e1: mcu.r[3] = arithmetic(mcu.r[3],0xffff,true); mcu.pc += 4; break;
        case 0x45eb: MCU_Write16(mcu,voice+20,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; break;
        case 0x45ee: case 0x4654: MCU_Write16(mcu,voice+10,0xffff); nz(0xffff); mcu.pc += 5; break;
        case 0x45f3: case 0x465c: mcu.r[5] = ReadWord(mcu,voice+86); nz(mcu.r[5]); mcu.pc += 3; break;
        case 0x45f6: mcu.pc = 0x465f; break;
        case 0x45f8: MCU_Write16(mcu,voice+10,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; break;
        case 0x45fb: mcu.r[5] = ReadWord(mcu,voice+84); nz(mcu.r[5]); mcu.pc += 3; break;
        case 0x45fe: branch(mcu.sr&STATUS_N,0x4607); break;
        case 0x4600: case 0x4607: mcu.r[2] = ReadWord(mcu,voice+86); nz(mcu.r[2]); mcu.pc += 3; break;
        case 0x4603: branch(mcu.sr&STATUS_N,0x4619); break;
        case 0x4605: mcu.pc = 0x4628; break;
        case 0x460a: branch(mcu.sr&STATUS_N,0x463a); break;
        case 0x460c: case 0x4628: case 0x463e: mcu.r[2] = arithmetic(mcu.r[2],mcu.r[5],true); mcu.pc += 2; break;
        case 0x460e: branch(!(mcu.sr&STATUS_N),0x4613); break;
        case 0x4610: case 0x461f: mcu.r[2] = 0x7fff; nz(mcu.r[2]); mcu.pc += 3; break;
        case 0x4615: case 0x4636: case 0x464e: mcu.r[5] = arithmetic(mcu.r[5],mcu.r[2]); mcu.pc += 2; break;
        case 0x4617: case 0x4626: case 0x4632: case 0x4638: case 0x464a: case 0x4652: mcu.pc = 0x465f; break;
        case 0x4619: case 0x462c: case 0x463a: case 0x4642: mcu.r[2] = arithmetic(0,mcu.r[2],true); mcu.pc += 2; break;
        case 0x461b: mcu.r[2] = arithmetic(mcu.r[2],mcu.r[5]); mcu.pc += 2; break;
        case 0x461d: branch(!(mcu.sr&STATUS_N),0x4622); break;
        case 0x4624: case 0x4630: case 0x4646: mcu.r[5] = arithmetic(mcu.r[5],mcu.r[2],true); mcu.pc += 2; break;
        case 0x462a: branch(!(mcu.sr&STATUS_C),0x4634); break;
        case 0x463c: case 0x4648: case 0x4650: mcu.r[5] = arithmetic(0,mcu.r[5],true); mcu.pc += 2; break;
        case 0x4640: branch(!(mcu.sr&STATUS_C),0x464c); break;
        case 0x4659: MCU_Write16(mcu,voice-48,mcu.r[6]); nz(mcu.r[6]); mcu.pc += 3; break;
        case 0x465f: MCU_Write16(mcu,voice+32,mcu.r[5]); nz(mcu.r[5]); mcu.pc += 3; break;
        case 0x4665: mcu.r[4] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x4667: mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|MCU_Read(mcu,voice+163)); nz(mcu.r[4],true); mcu.pc += 4; break;
        case 0x466b: mcu.r[2] = ReadWord(mcu,voice+46); nz(mcu.r[2]); mcu.pc += 3; break;
        case 0x466e:
            if (mcu.r[2] < 0x8000 || unsigned(mcu.r[2])+18 >= 0xe000) return false;
            mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|MCU_Read(mcu,mcu.r[2]+18)); nz(mcu.r[6],true); mcu.pc += 3; break;
        case 0x4671: mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|arithmetic(mcu.r[6],64,true,true)); mcu.pc += 3; break;
        case 0x4674: branch(!(mcu.sr&STATUS_C),0x4680); break;
        case 0x4676: mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|arithmetic(0,mcu.r[6],true,true)); mcu.pc += 2; break;
        case 0x4678: mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|arithmetic(mcu.r[4],mcu.r[6],true,true)); mcu.pc += 2; break;
        case 0x467a: case 0x468e: branch(!(mcu.sr&STATUS_N),0x4692); break;
        case 0x467c: mcu.r[4] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x467e: mcu.pc = 0x4692; break;
        case 0x4680:
            mcu.sr = uint16_t((mcu.sr&~STATUS_Z)|((MCU_Read(mcu,voice+162)&4) ? 0 : STATUS_Z)); mcu.pc += 4; break;
        case 0x4684: branch(!(mcu.sr&STATUS_Z),0x4692); break;
        case 0x4686: arithmetic(mcu.r[6],16,true,true); mcu.pc += 2; break;
        case 0x4688: branch(mcu.sr&STATUS_C,0x468c); break;
        case 0x468a: mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|16); nz(mcu.r[6],true); mcu.pc += 2; break;
        case 0x468c: mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|arithmetic(mcu.r[4],mcu.r[6],false,true)); mcu.pc += 2; break;
        case 0x4690: mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|127); nz(mcu.r[4],true); mcu.pc += 2; break;
        case 0x4692: mcu.r[4] = uint16_t((mcu.r[4]<<8)|(mcu.r[4]>>8)); nz(mcu.r[4]); mcu.pc += 2; break;
        case 0x4694: nz(mcu.r[5]); mcu.sr &= ~STATUS_C; mcu.pc += 2; break;
        case 0x4696: branch(mcu.sr&STATUS_N,0x46a1); break;
        case 0x4698: case 0x46a1: mcu.r[5] = arithmetic(mcu.r[5],mcu.r[4]); mcu.pc += 2; break;
        case 0x469a: case 0x46a3: branch(!(mcu.sr&STATUS_N),0x46a7); break;
        case 0x469c: case 0x46c0: mcu.r[5] = 0x7fff; nz(mcu.r[5]); mcu.pc += 3; break;
        case 0x469f: mcu.pc = 0x46a7; break;
        case 0x46a5: case 0x46b8: mcu.r[5] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x46a7: MCU_Write16(mcu,voice+34,mcu.r[5]); nz(mcu.r[5]); mcu.pc += 3; break;
        case 0x46aa: mcu.r[2] = ReadWord(mcu,voice+136); nz(mcu.r[2]); mcu.pc += 4; break;
        case 0x46ae: branch(mcu.sr&STATUS_Z,0x46c3); break;
        case 0x46b0: branch(!(mcu.sr&STATUS_N),0x46bc); break;
        case 0x46b2: mcu.r[2] = arithmetic(0,mcu.r[2],true); mcu.pc += 2; break;
        case 0x46b4: mcu.r[5] = arithmetic(mcu.r[5],mcu.r[2],true); mcu.pc += 2; break;
        case 0x46b6: branch(!(mcu.sr&STATUS_C),0x46c3); break;
        case 0x46ba: mcu.pc = 0x46c3; break;
        case 0x46bc: mcu.r[5] = arithmetic(mcu.r[5],mcu.r[2]); mcu.pc += 2; break;
        case 0x46be: branch(!(mcu.sr&STATUS_V),0x46c3); break;
        case 0x46e0: mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|MCU_Read(mcu,voice+105)); nz(mcu.r[6],true); mcu.pc += 3; break;
        case 0x46e3: mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|MCU_Read(mcu,voice+103)); nz(mcu.r[4],true); mcu.pc += 3; break;
        case 0x46e6: mcu.r[2] = ReadWord(mcu,voice+46); nz(mcu.r[2]); mcu.pc += 3; break;
        case 0x46e9:
            if (mcu.r[2] < 0x8000 || unsigned(mcu.r[2])+19 >= 0xe000) return false;
            mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|MCU_Read(mcu,mcu.r[2]+19)); nz(mcu.r[5],true); mcu.pc += 3; break;
        case 0x46ec: case 0x4718: {
            const unsigned delta = mcu.pc == 0x46ec ? 64 : 1;
            mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|arithmetic(mcu.r[5],delta,true,true)); mcu.pc += 3; break;
        }
        case 0x46ef: branch(mcu.sr&STATUS_N,0x46ff); break;
        case 0x46f1: case 0x4701:
            mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|arithmetic(mcu.r[5],mcu.r[5],false,true)); mcu.pc += 2; break;
        case 0x46f3: mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|arithmetic(mcu.r[4],mcu.r[5],true,true)); mcu.pc += 2; break;
        case 0x46f5: branch(mcu.sr&STATUS_N,0x470d); break;
        case 0x46f7: case 0x4705: arithmetic(mcu.r[4],mcu.r[6],true,true); mcu.pc += 2; break;
        case 0x46f9: case 0x4707: branch(mcu.sr&STATUS_C,0x470f); break;
        case 0x46fb: case 0x4709: mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|(mcu.r[6]&255)); nz(mcu.r[4],true); mcu.pc += 2; break;
        case 0x46fd: case 0x470b: mcu.pc = 0x470f; break;
        case 0x46ff: mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|arithmetic(0,mcu.r[5],true,true)); mcu.pc += 2; break;
        case 0x4703: mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|arithmetic(mcu.r[4],mcu.r[5],false,true)); mcu.pc += 2; break;
        case 0x470d: mcu.r[4] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x470f: mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|MCU_Read(mcu,voice+104)); nz(mcu.r[5],true); mcu.pc += 3; break;
        case 0x4712: arithmetic(mcu.r[4],mcu.r[5],true,true); mcu.pc += 2; break;
        case 0x4714: branch(mcu.sr&STATUS_Z,0x473c); break;
        case 0x4716: branch(!(mcu.sr&STATUS_C),0x471d); break;
        case 0x471b: mcu.pc = 0x471f; break;
        case 0x471d: mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|arithmetic(mcu.r[5],1,false,true)); mcu.pc += 2; break;
        case 0x471f: mcu.r[3] = ReadWord(mcu,voice+36); nz(mcu.r[3]); mcu.pc += 3; break;
        case 0x4722: mcu.r[3] = arithmetic(mcu.r[3],255); mcu.pc += 4; break;
        case 0x4726: branch(!(mcu.sr&STATUS_C),0x472b); break;
        case 0x4728: mcu.r[3] = 0xff00; nz(mcu.r[3]); mcu.pc += 3; break;
        case 0x472b: mcu.r[3] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x472d: mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; break;
        case 0x472f:
            if (mcu.r[3] > 255) return false;
            mcu.r[3] = MCU_Read(mcu,0x7714+mcu.r[3]); nz(mcu.r[3],true); mcu.pc += 4; break;
        case 0x4733: arithmetic(mcu.r[3],mcu.r[5],true,true); mcu.pc += 2; break;
        case 0x4735: branch(!(mcu.sr&STATUS_C),0x4739); break;
        case 0x4737: mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|(mcu.r[3]&255)); nz(mcu.r[5],true); mcu.pc += 2; break;
        case 0x4739: MCU_Write(mcu,voice+104,uint8_t(mcu.r[5])); nz(mcu.r[5],true); mcu.pc += 3; break;
        case 0x473c: mcu.r[4] = ReadWord(mcu,voice+34); nz(mcu.r[4]); mcu.pc += 3; break;
        case 0x473f: mcu.r[3] = mcu.r[4]; nz(mcu.r[3]); mcu.pc += 2; break;
        case 0x4741: mcu.r[3] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x4743: case 0x477b: mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; break;
        case 0x4745: mcu.r[3] = arithmetic(mcu.r[3],mcu.r[3]); mcu.pc += 2; break;
        case 0x4747: case 0x4755: {
            if (mcu.r[3] > 510) return false;
            const unsigned reg = mcu.pc == 0x4747 ? 6 : 5;
            mcu.r[reg] = ReadWord(mcu,0x7612+mcu.r[3]); nz(mcu.r[reg]); mcu.pc += 4; break;
        }
        case 0x474b: nz(mcu.r[4],true); mcu.sr &= ~STATUS_C; mcu.pc += 2; break;
        case 0x474d: branch(mcu.sr&STATUS_Z,0x4763); break;
        case 0x474f: mcu.r[4] &= 255; nz(mcu.r[4]); mcu.pc += 4; break;
        case 0x4753: mcu.r[3] = arithmetic(mcu.r[3],2); mcu.pc += 2; break;
        case 0x4759: mcu.r[5] = arithmetic(mcu.r[5],mcu.r[6],true); mcu.pc += 2; break;
        case 0x475b: {
            const uint32_t product = uint32_t(mcu.r[4])*mcu.r[5];
            mcu.r[4] = uint16_t(product>>16); mcu.r[5] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += 2; break;
        }
        case 0x475d: mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|(mcu.r[4]&255)); nz(mcu.r[5],true); mcu.pc += 2; break;
        case 0x475f: mcu.r[5] = uint16_t((mcu.r[5]<<8)|(mcu.r[5]>>8)); nz(mcu.r[5]); mcu.pc += 2; break;
        case 0x4761: mcu.r[6] = arithmetic(mcu.r[6],mcu.r[5]); mcu.pc += 2; break;
        case 0x4763: mcu.r[6] = arithmetic(mcu.r[6],mcu.r[6]); mcu.pc += 2; break;
        case 0x4765: mcu.r[5] = ReadWord(mcu,voice+36); nz(mcu.r[5]); mcu.pc += 3; break;
        case 0x4768: arithmetic(MCU_Read(mcu,voice+104),8,true,true); mcu.pc += 4; break;
        case 0x476c: branch(!(mcu.sr&STATUS_C),0x4772); break;
        case 0x476e: MCU_Write(mcu,voice+104,8); nz(8,true); mcu.pc += 4; break;
        case 0x4772: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x4774: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|MCU_Read(mcu,voice+104)); nz(mcu.r[3],true); mcu.pc += 3; break;
        case 0x4777:
            if (mcu.r[3] > 255) return false;
            mcu.r[3] = MCU_Read(mcu,0x7816+mcu.r[3]); nz(mcu.r[3],true); mcu.pc += 4; break;
        case 0x477d: arithmetic(mcu.r[3],mcu.r[6],true); mcu.pc += 2; break;
        case 0x477f: branch(!(mcu.sr&STATUS_C),0x4783); break;
        case 0x4781: mcu.r[6] = mcu.r[3]; nz(mcu.r[6]); mcu.pc += 2; break;
        case 0x4783: arithmetic(mcu.r[6],0xe600,true); mcu.pc += 3; break;
        case 0x4786: branch(mcu.sr&(STATUS_C|STATUS_Z),0x478b); break;
        case 0x4788: mcu.r[6] = 0xe600; nz(mcu.r[6]); mcu.pc += 3; break;
        case 0x478b: MCU_Write16(mcu,voice+36,mcu.r[6]); nz(mcu.r[6]); mcu.pc += 3; break;
        case 0x478e: mcu.r[2] = mcu.r[6]; nz(mcu.r[2]); mcu.pc += 2; break;
        case 0x4790: mcu.r[2] = arithmetic(mcu.r[2],mcu.r[5],true); mcu.pc += 2; break;
        case 0x4792: case 0x47e7: branch(mcu.sr&STATUS_Z,0x47ef); break;
        case 0x4794: branch(!(mcu.sr&STATUS_C),0x479a); break;
        case 0x4796: mcu.r[2] = arithmetic(0,mcu.r[2],true); mcu.pc += 2; break;
        case 0x4798: mcu.pc = 0x47b7; break;
        case 0x479a: case 0x479c: {
            const unsigned reg = mcu.pc == 0x479a ? 6 : 5;
            mcu.r[reg] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        }
        case 0x479e: arithmetic(mcu.r[6],mcu.r[5],true); mcu.pc += 2; break;
        case 0x47a0: branch(!(mcu.sr&STATUS_Z),0x47b7); break;
        case 0x47a2: mcu.r[6] = arithmetic(mcu.r[6],256); mcu.pc += 4; break;
        case 0x47a6: case 0x47c4: case 0x47d1:
            mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x47a8: mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|MCU_Read(mcu,voice+104)); nz(mcu.r[3],true); mcu.pc += 3; break;
        case 0x47ab:
            if (mcu.r[3] > 255) return false;
            mcu.r[3] = MCU_Read(mcu,0x7816+mcu.r[3]); nz(mcu.r[3],true); mcu.pc += 4; break;
        case 0x47af: mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; break;
        case 0x47b1: arithmetic(mcu.r[3],mcu.r[6],true); mcu.pc += 2; break;
        case 0x47b3: branch(!(mcu.sr&STATUS_C),0x47b7); break;
        case 0x47b5: mcu.r[6] = mcu.r[3]; nz(mcu.r[6]); mcu.pc += 2; break;
        case 0x47b7: arithmetic(mcu.r[6],0xe600,true); mcu.pc += 3; break;
        case 0x47ba: branch(mcu.sr&(STATUS_C|STATUS_Z),0x47bf); break;
        case 0x47bc: mcu.r[6] = 0xe600; nz(mcu.r[6]); mcu.pc += 3; break;
        case 0x47bf: mcu.r[1] = ReadWord(mcu,voice-48); nz(mcu.r[1]); mcu.pc += 3; break;
        case 0x47c2: branch(mcu.sr&STATUS_Z,0x47f5); break;
        case 0x47c6:
            if (mcu.r[1] > 8) return false;
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|MCU_Read(mcu,0x6b06+mcu.r[1])); nz(mcu.r[3],true); mcu.pc += 4; break;
        case 0x47ca: {
            const bool carry = (mcu.r[2]&0x8000) != 0;
            mcu.r[2] = uint16_t(mcu.r[2]<<1); nz(mcu.r[2]);
            mcu.sr = uint16_t((mcu.sr&~STATUS_C)|(carry ? STATUS_C : 0)); mcu.pc += 2; break;
        }
        case 0x47cc: branch(mcu.sr&STATUS_C,0x47d5); break;
        case 0x47ce:
            mcu.pc += 3;
            if (!(mcu.sr&STATUS_Z)) { --mcu.r[3]; if (mcu.r[3] != 0xffff) mcu.pc = 0x47ca; }
            break;
        case 0x47d3: case 0x47d9: case 0x47db: case 0x47dd: case 0x47e1: {
            const bool byte = mcu.pc != 0x47d3, carry = (mcu.r[2]&1) != 0;
            mcu.r[2] = byte ? uint16_t((mcu.r[2]&0xff00)|((mcu.r[2]&255)>>1)) : uint16_t(mcu.r[2]>>1);
            nz(mcu.r[2],byte); mcu.sr = uint16_t((mcu.sr&~STATUS_C)|(carry ? STATUS_C : 0)); mcu.pc += 2; break;
        }
        case 0x47d5: mcu.r[2] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x47d7: mcu.r[2] = uint16_t((mcu.r[2]<<8)|(mcu.r[2]>>8)); nz(mcu.r[2]); mcu.pc += 2; break;
        case 0x47df: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|arithmetic(mcu.r[2],1,false,true)); mcu.pc += 2; break;
        case 0x47e3:
            if (mcu.r[3] > 10) return false;
            mcu.r[2] |= MCU_Read(mcu,0x67ba+mcu.r[3]); nz(mcu.r[2],true); mcu.pc += 4; break;
        case 0x47e9: mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|(mcu.r[2]&255)); nz(mcu.r[6],true); mcu.pc += 2; break;
        case 0x47eb: case 0x47f7: MCU_Write16(mcu,voice+38,mcu.r[6]); nz(mcu.r[6]); mcu.pc += 3; break;
        case 0x47ef: MCU_Write16(mcu,voice+38,0xff00); nz(0xff00); mcu.pc += 5; break;
        case 0x47f5: mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|0xaf); nz(mcu.r[6],true); mcu.pc += 2; break;
        default: return false;
    }
    return true;
}

// Direct entry to the level LFO helper, with its outer RTS left separate.
inline bool TryModulateLevel(mcu_t& mcu)
{
    if (!mcu.native_v121_enabled || mcu.cp || mcu.pc != 0x312b
        || (mcu.sr&STATUS_T) || (mcu.sr&0x0700) != 0x0700) return false;
    const uint16_t previous = mcu.r[4], a = mcu.r[2], b = mcu.r[3];
    const bool aNegative = (a&0x8000) != 0, bNegative = (b&0x8000) != 0;
    const bool negative = aNegative == bNegative ? aNegative : (uint16_t(a+b)&0x8000) != 0;
    const bool subtract = negative != ((mcu.r[6]&0x8000) != 0);
    const unsigned count = ModulateV121(mcu.r[4],a,b,mcu.r[6],mcu.r[2],mcu.r[3]);
    const unsigned carry = uint16_t(mcu.r[3]+1) != 0 ? 1 : 0;
    const unsigned delta = unsigned(mcu.r[2])+carry;
    const bool carryOut = subtract ? previous < delta : unsigned(previous)+delta > 65535;
    if (subtract && carryOut) mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z);
    else {
        const int signedResult = subtract ? int(int16_t(previous))-int(int16_t(mcu.r[2]))-int(carry)
                                          : int(int16_t(previous))+int(int16_t(mcu.r[2]))+int(carry);
        mcu.sr = uint16_t((mcu.sr&~15)|(mcu.r[4]&0x8000 ? STATUS_N : 0)
            |(!mcu.r[4] && (subtract || !mcu.r[3]) ? STATUS_Z : 0)
            |(carryOut ? STATUS_C : 0)|(signedResult < -32768 || signedResult > 32767 ? STATUS_V : 0));
    }
    mcu.pc = 0x3187; mcu.native_debt = count-2;
    return true;
}

// Interruptible level LFO composition, retaining the original stack/IRQ boundaries.
inline bool TryStepLevelModulation(mcu_t& mcu)
{
    if (!mcu.native_v121_enabled || mcu.cp || (mcu.sr&STATUS_T)) return false;
    auto arithmetic = [&](uint16_t a, uint16_t b, bool sub = false, unsigned carry = 0) {
        const uint16_t value = uint16_t(sub ? a-b-carry : a+b+carry);
        const int signedValue = sub ? int(int16_t(a))-int(int16_t(b))-int(carry)
                                    : int(int16_t(a))+int(int16_t(b))+int(carry);
        mcu.sr = uint16_t((mcu.sr&~15)|(value&0x8000 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
            |((sub ? unsigned(a) < unsigned(b)+carry : unsigned(a)+b+carry > 65535) ? STATUS_C : 0)
            |(signedValue < -32768 || signedValue > 32767 ? STATUS_V : 0));
        return value;
    };
    auto test = [&](unsigned reg) { const auto value = mcu.r[reg];
        mcu.sr = uint16_t((mcu.sr&~15)|(value&0x8000 ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto branch = [&](bool take, uint16_t target) { mcu.pc = take ? target : uint16_t(mcu.pc+2); };
    switch (mcu.pc) {
        case 0x312b: test(2); mcu.pc += 2; break;
        case 0x312d: branch(mcu.sr&STATUS_N,0x313f); break;
        case 0x312f: test(3); mcu.pc += 2; break;
        case 0x3131: branch(mcu.sr&STATUS_N,0x3153); break;
        case 0x3133: mcu.r[2] = arithmetic(mcu.r[2],mcu.r[3]); mcu.pc += 2; break;
        case 0x3135: arithmetic(mcu.r[2],0x7f00,true); mcu.pc += 3; break;
        case 0x3138: branch(mcu.sr&(STATUS_C|STATUS_Z),0x315b); break;
        case 0x313a: mcu.r[2] = 0x7f00; mcu.sr &= ~(STATUS_N|STATUS_Z|STATUS_V); mcu.pc += 3; break;
        case 0x313d: mcu.pc = 0x315b; break;
        case 0x313f: test(3); mcu.pc += 2; break;
        case 0x3141: branch(!(mcu.sr&STATUS_N),0x3153); break;
        case 0x3145: mcu.r[2] = arithmetic(0,mcu.r[2],true); mcu.pc += 2; break;
        case 0x3143: mcu.r[3] = arithmetic(0,mcu.r[3],true); mcu.pc += 2; break;
        case 0x3149: arithmetic(mcu.r[2],0x7f00,true); mcu.pc += 3; break;
        case 0x314c: branch(mcu.sr&(STATUS_C|STATUS_Z),0x3163); break;
        case 0x314e: mcu.r[2] = 0x7f00; mcu.sr &= ~(STATUS_N|STATUS_Z|STATUS_V); mcu.pc += 3; break;
        case 0x3151: mcu.pc = 0x3163; break;
        case 0x3153: mcu.r[2] = arithmetic(mcu.r[2],mcu.r[3]); mcu.pc += 2; break;
        case 0x3155: branch(!(mcu.sr&STATUS_N),0x315b); break;
        case 0x3157: mcu.r[2] = arithmetic(0,mcu.r[2],true); mcu.pc += 2; break;
        case 0x3159: mcu.pc = 0x3163; break;
        case 0x315b: test(6); mcu.pc += 2; break;
        case 0x315d: branch(!(mcu.sr&STATUS_N),0x3169); break;
        case 0x315f: mcu.r[6] = arithmetic(0,mcu.r[6],true); mcu.pc += 2; break;
        case 0x3161: mcu.pc = 0x3177; break;
        case 0x3163: test(6); mcu.pc += 2; break;
        case 0x3165: branch(!(mcu.sr&STATUS_N),0x3177); break;
        case 0x3167: mcu.r[6] = arithmetic(0,mcu.r[6],true); mcu.pc += 2; break;
        case 0x3169: {
            const uint32_t product = uint32_t(mcu.r[2])*mcu.r[6];
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += 2; break;
        }
        case 0x3177: {
            const uint32_t product = uint32_t(mcu.r[2])*mcu.r[6];
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += 2; break;
        }
        case 0x3183: branch(!(mcu.sr&STATUS_C),0x3187); break;
        case 0x3147: mcu.r[2] = arithmetic(mcu.r[2],mcu.r[3]); mcu.pc += 2; break;
        case 0x316b: case 0x3179: mcu.r[3] = arithmetic(mcu.r[3],mcu.r[3]); mcu.pc += 2; break;
        case 0x316d: case 0x317b: {
            const bool zero = (mcu.sr&STATUS_Z) != 0;
            mcu.r[2] = arithmetic(mcu.r[2],mcu.r[2],false,(mcu.sr&STATUS_C) ? 1 : 0);
            if (!zero) mcu.sr &= ~STATUS_Z;
            mcu.pc += 2; break;
        }
        case 0x316f: case 0x317d: mcu.r[3] = arithmetic(mcu.r[3],0xffff); mcu.pc += 4; break;
        case 0x3173: case 0x3181: {
            const bool sub = mcu.pc == 0x3181, zero = (mcu.sr&STATUS_Z) != 0;
            mcu.r[4] = arithmetic(mcu.r[4],mcu.r[2],sub,(mcu.sr&STATUS_C) ? 1 : 0);
            if (!sub && !zero) mcu.sr &= ~STATUS_Z;
            mcu.pc += 2; break;
        }
        case 0x3175: mcu.pc = 0x3187; break;
        case 0x3185: mcu.r[4] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; break;
        case 0x3187: ++mcu.pc; mcu.pc = MCU_PopStack(mcu); break;
        default: return false;
    }
    return true;
}

// Level LFO call boundaries and final output scaling, one instruction per step.
inline bool TryStepLevelConnections(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~(STATUS_N|STATUS_Z|STATUS_V))
            |(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto arithmetic = [&](uint16_t a, uint16_t b, bool subtract = false, unsigned carry = 0) {
        const unsigned wide = subtract ? unsigned(a)-b-carry : unsigned(a)+b+carry;
        const uint16_t value = uint16_t(wide);
        const int signedValue = subtract ? int(int16_t(a))-int(int16_t(b))-int(carry)
                                         : int(int16_t(a))+int(int16_t(b))+int(carry);
        mcu.sr = uint16_t((mcu.sr&~15)|(value&0x8000 ? STATUS_N : 0)
            |(!value ? STATUS_Z : 0)|(wide&0x10000 ? STATUS_C : 0)
            |(signedValue < -32768 || signedValue > 32767 ? STATUS_V : 0));
        return value;
    };
    unsigned reg, address, length;
    switch (mcu.pc) {
        case 0x309b: case 0x30ab:
            mcu.r[mcu.pc == 0x309b ? 2 : 6] = 0;
            mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x309d: case 0x30a1: case 0x30ad: case 0x30c6: {
            const unsigned at = mcu.pc == 0x309d ? uint16_t(0xc8e4+mcu.r[1])
                : mcu.pc == 0x30a1 ? uint16_t(0xab36+mcu.r[2])
                : mcu.pc == 0x30ad ? 0x8002 : uint16_t(mcu.r[3]+0x100);
            if (at < 0x8000 || at > 0xdfff) return false;
            const unsigned dest = mcu.pc == 0x309d || mcu.pc == 0x30a1 ? 2 : 6;
            mcu.r[dest] = uint16_t((mcu.r[dest]&0xff00)|MCU_Read(mcu,at));
            nz(mcu.r[dest],true); mcu.pc += 4; return true;
        }
        case 0x30a5: reg = 3; address = voice+46; length = 3; break;
        case 0x30c1: reg = 3; address = voice+48; length = 3; break;
        case 0x30a8: {
            const unsigned at = uint16_t(mcu.r[3]+8);
            if (at < 0x8000 || at > 0xdfff) return false;
            mcu.r[2] = uint16_t((mcu.r[2]&255)*MCU_Read(mcu,at));
            mcu.sr &= ~STATUS_C; nz(mcu.r[2]); mcu.pc += 3; return true;
        }
        case 0x30bb: case 0x30d0:
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|(mcu.r[2]&255));
            nz(mcu.r[3],true); mcu.pc += 2; return true;
        case 0x30bd: case 0x30d2:
            mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8));
            nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x30bf: case 0x30d4:
            mcu.r[2] = mcu.r[3]; nz(mcu.r[2]); mcu.pc += 2; return true;
        case 0x30c4: mcu.pc = (mcu.sr&STATUS_Z) ? 0x30dc : 0x30c6; return true;
        case 0x30b1: case 0x30ca: case 0x30d6: case 0x30dc: {
            const bool immediate = mcu.pc == 0x30d6 || mcu.pc == 0x30dc;
            const uint32_t product = uint32_t(mcu.r[2])*(immediate ? (mcu.pc == 0x30d6 ? 0x830e : 0x8208) : mcu.r[6]);
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += immediate ? 4 : 2; return true;
        }
        case 0x30da: mcu.pc = 0x30e0; return true;
        case 0x30b3: case 0x30b7: case 0x30cc: case 0x30e0:
            mcu.r[3] = arithmetic(mcu.r[3],mcu.r[3]); mcu.pc += 2; return true;
        case 0x30b5: case 0x30b9: case 0x30ce: case 0x30e2: {
            const bool previousZero = (mcu.sr&STATUS_Z) != 0;
            mcu.r[2] = arithmetic(mcu.r[2],mcu.r[2],false,(mcu.sr&STATUS_C) != 0);
            if (!previousZero) mcu.sr &= ~STATUS_Z;
            mcu.pc += 2; return true;
        }
        case 0x30e4: mcu.r[4] = mcu.r[2]; nz(mcu.r[4]); mcu.pc += 2; return true;
        case 0x30e6: mcu.pc = (mcu.sr&STATUS_Z) ? 0x30e8 : 0x30eb; return true;
        case 0x30e8: mcu.r[5] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x30eb: reg = 2; address = voice+138; length = 4; break;
        case 0x30ef: mcu.pc = (mcu.sr&STATUS_Z) ? 0x30ff : 0x30f1; return true;
        case 0x30f1: mcu.pc = (mcu.sr&STATUS_N) ? 0x30f3 : 0x30fd; return true;
        case 0x30f3: mcu.r[2] = arithmetic(0,mcu.r[2],true); mcu.pc += 2; return true;
        case 0x30f5: mcu.r[4] = arithmetic(mcu.r[4],mcu.r[2],true); mcu.pc += 2; return true;
        case 0x30f7: mcu.pc = (mcu.sr&STATUS_C) ? 0x30f9 : 0x30ff; return true;
        case 0x30f9: mcu.r[4] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x30fb: mcu.pc = 0x30ff; return true;
        case 0x30fd: mcu.r[4] = arithmetic(mcu.r[4],mcu.r[2]); mcu.pc += 2; return true;
        case 0x30ff: reg = 2; address = voice-122; length = 3; break;
        case 0x3102: reg = 3; address = voice+142; length = 4; break;
        case 0x3106: reg = 6; address = voice-96; length = 3; break;
        case 0x310b: reg = 2; address = voice-88; length = 3; break;
        case 0x310e: reg = 3; address = voice+150; length = 4; break;
        case 0x3112: reg = 6; address = voice-62; length = 3; break;
        case 0x3109: case 0x3115:
            mcu.pc += 2; MCU_PushStack(mcu,mcu.pc); mcu.pc = 0x312b; return true;
        case 0x30ea: case 0x3126: case 0x312a:
            ++mcu.pc; mcu.pc = MCU_PopStack(mcu); return true;
        case 0x3117: case 0x3119: {
            const uint32_t product = uint32_t(mcu.r[4])*(mcu.pc == 0x3117 ? mcu.r[4] : 0x208);
            mcu.r[4] = uint16_t(product>>16); mcu.r[5] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += mcu.pc == 0x3117 ? 2 : 4; return true;
        }
        case 0x311d: {
            const unsigned value = mcu.r[4]; const uint16_t difference = uint16_t(value-255);
            const int signedDifference = int(int16_t(value))-255;
            mcu.sr = uint16_t((mcu.sr&~15)|(difference&0x8000 ? STATUS_N : 0)
                |(!difference ? STATUS_Z : 0)|(value < 255 ? STATUS_C : 0)
                |(signedDifference < -32768 ? STATUS_V : 0));
            mcu.pc += 3; return true;
        }
        case 0x3120: mcu.pc = (mcu.sr&STATUS_C) ? 0x3122 : 0x3127; return true;
        case 0x3122: mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|(mcu.r[4]&255)); nz(mcu.r[5],true); mcu.pc += 2; return true;
        case 0x3124: mcu.r[5] = uint16_t((mcu.r[5]<<8)|(mcu.r[5]>>8)); nz(mcu.r[5]); mcu.pc += 2; return true;
        case 0x3127: mcu.r[5] = 0xffff; nz(mcu.r[5]); mcu.pc += 3; return true;
        default: return false;
    }
    mcu.pc = uint16_t(mcu.pc+length); mcu.r[reg] = ReadWord(mcu,address); nz(mcu.r[reg]);
    return true;
}

// Voice output staging and PCM accesses, each at its original instruction boundary.
inline bool TryStepVoiceOutput(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](uint16_t value, bool byte = false) {
        if (byte) value &= 255;
        mcu.sr = uint16_t((mcu.sr&~(STATUS_N|STATUS_Z|STATUS_V))
            |(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto compare = [&](uint16_t value, uint16_t rhs, bool byte = false) {
        if (byte) value &= 255;
        const unsigned mask = byte ? 255 : 65535, sign = byte ? 128 : 32768;
        const unsigned result = (unsigned(value)-rhs)&mask;
        mcu.sr = uint16_t((mcu.sr&~15)|(result&sign ? STATUS_N : 0)|(!result ? STATUS_Z : 0)
            |(value < rhs ? STATUS_C : 0)|((value^rhs)&(value^result)&sign ? STATUS_V : 0));
    };
    unsigned offset = 0;
    switch (mcu.pc) {
        case 0x5855: case 0x318c:
            MCU_ControlRegisterWrite(mcu,0,MCU_Operand_Size::WORD,mcu.sr|0x0700);
            mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x3190:
            mcu.pc += 5; MCU_Write(mcu,voice-26,255); nz(255,true); return true;
        case 0x3195: compare(ReadWord(mcu,voice),14); mcu.pc += 5; return true;
        case 0x319a: mcu.pc = (mcu.sr&STATUS_C) ? 0x319d : 0x3363; return true;
        case 0x31a7:
            if (mcu.br != 0xe0) return false;
            mcu.pc += 2; MCU_Write(mcu,0xe03e,uint8_t(mcu.r[1])); nz(mcu.r[1],true); return true;
        case 0x31b0: case 0x31ce: case 0x31fb:
            if (mcu.br != 0xe0) return false;
            offset = mcu.pc == 0x31b0 ? 0x16 : mcu.pc == 0x31ce ? 0x18 : 0x1a;
            mcu.pc += 5; MCU_Write16(mcu,0xe000+offset,0xff00); nz(0xff00); return true;
        case 0x31b5: case 0x31d3: case 0x3200:
            if (mcu.br != 0xe0) return false;
            offset = mcu.pc == 0x31b5 ? 0x32 : mcu.pc == 0x31d3 ? 0x34 : 0x36;
            mcu.pc += 2; mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|MCU_Read(mcu,0xe000+offset));
            nz(mcu.r[5],true); return true;
        case 0x31b7: case 0x31d5: case 0x3202:
            if (mcu.br != 0xe0) return false;
            mcu.pc += 2; mcu.r[5] = MCU_Read16(mcu,0xe03a); nz(mcu.r[5]); return true;
        case 0x31c5: case 0x31e3: case 0x3210:
            if (mcu.br != 0xe0) return false;
            offset = mcu.pc == 0x31c5 ? 0x32 : mcu.pc == 0x31e3 ? 0x34 : 0x36;
            mcu.pc += 2; MCU_Write16(mcu,0xe000+offset,mcu.r[5]); nz(mcu.r[5]); return true;
        case 0x5894:
            MCU_ControlRegisterWrite(mcu,0,MCU_Operand_Size::WORD,mcu.sr&0xf8ff);
            mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x5898:
            if ((mcu.r[7]&1) || mcu.r[7] < 0x8000 || mcu.r[7] > 0xdffe) return false;
            ++mcu.pc; mcu.pc = MCU_PopStack(mcu); return true;
        case 0x5859: case 0x319d: compare(ReadWord(mcu,voice),0); mcu.pc += 5; return true;
        case 0x585e: mcu.pc = (mcu.sr&STATUS_Z) ? 0x5894 : 0x5860; return true;
        case 0x5860: compare(ReadWord(mcu,voice),14); mcu.pc += 5; return true;
        case 0x5865: mcu.pc = (mcu.sr&STATUS_C) ? 0x5867 : 0x5894; return true;
        case 0x5867: mcu.r[3] = ReadWord(mcu,voice-2); nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x586a:
            if (mcu.br != 0xe0) return false;
            mcu.pc += 2; MCU_Write(mcu,0xe03e,uint8_t(mcu.r[3])); nz(mcu.r[3],true); return true;
        case 0x586c: case 0x5871: case 0x5876: case 0x587b: case 0x5880: case 0x588f:
            offset = mcu.pc == 0x586c ? 30 : mcu.pc == 0x5871 ? 26 : mcu.pc == 0x5876 ? 52
                : mcu.pc == 0x587b ? 58 : mcu.pc == 0x5880 ? 38 : 72;
            mcu.r[6] = ReadWord(mcu,voice+offset); nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x5885: case 0x588a:
            offset = mcu.pc == 0x5885 ? 104 : 102;
            mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|MCU_Read(mcu,voice+offset));
            nz(mcu.r[6],true); mcu.pc += 3; return true;
        case 0x5888:
            mcu.r[6] = uint16_t((mcu.r[6]<<8)|(mcu.r[6]>>8)); nz(mcu.r[6]); mcu.pc += 2; return true;
        case 0x586f: case 0x5874: case 0x5879: case 0x587e: case 0x5883: case 0x588d: case 0x5892:
            if (mcu.br != 0xe0) return false;
            offset = mcu.pc == 0x586f ? 0x18 : mcu.pc == 0x5874 ? 0x16 : mcu.pc == 0x5879 ? 0x12
                : mcu.pc == 0x587e ? 0x14 : mcu.pc == 0x5883 ? 0x1a : mcu.pc == 0x588d ? 0x1c : 0x10;
            mcu.pc += 2; MCU_Write16(mcu,0xe000+offset,mcu.r[6]); nz(mcu.r[6]); return true;
        case 0x31a2: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3212 : 0x31a4; return true;
        case 0x31a4: mcu.r[1] = ReadWord(mcu,voice-2); nz(mcu.r[1]); mcu.pc += 3; return true;
        case 0x31a9: case 0x31c7: case 0x31f4:
            offset = mcu.pc == 0x31a9 ? 26 : mcu.pc == 0x31c7 ? 30 : 38;
            compare(ReadWord(mcu,voice+offset),0xff00); mcu.pc += 5; return true;
        case 0x31ae: mcu.pc = (mcu.sr&STATUS_Z) ? 0x31c0 : 0x31b0; return true;
        case 0x31cc: mcu.pc = (mcu.sr&STATUS_Z) ? 0x31de : 0x31ce; return true;
        case 0x31f9: mcu.pc = (mcu.sr&STATUS_Z) ? 0x320b : 0x31fb; return true;
        case 0x31b9: case 0x31d7: case 0x3204: {
            const unsigned previous = mcu.r[5], result = previous*2;
            mcu.r[5] = uint16_t(result); nz(mcu.r[5]);
            mcu.sr = uint16_t((mcu.sr&~(STATUS_C|STATUS_V))|(result&65536 ? STATUS_C : 0)
                |((previous^result)&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x31bb: case 0x31d9: case 0x3206:
            offset = mcu.pc == 0x31bb ? 24 : mcu.pc == 0x31d9 ? 28 : 36;
            MCU_Write(mcu,voice+offset,uint8_t(mcu.r[5]>>8));
            MCU_Write(mcu,voice+offset+1,uint8_t(mcu.r[5]));
            nz(mcu.r[5]); mcu.pc += 3; return true;
        case 0x31be: mcu.pc = 0x31c7; return true;
        case 0x31dc: mcu.pc = 0x31e5; return true;
        case 0x3209: mcu.pc = 0x3212; return true;
        case 0x31c0: case 0x31de: case 0x31e5: case 0x320b:
            offset = mcu.pc == 0x31c0 ? 24 : mcu.pc == 0x320b ? 36 : 28;
            mcu.r[5] = ReadWord(mcu,voice+offset); nz(mcu.r[5]); mcu.pc += 3; return true;
        case 0x31c3: case 0x31e1: case 0x320e: {
            const bool carry = (mcu.r[5]&1) != 0;
            mcu.r[5] >>= 1; nz(mcu.r[5]);
            mcu.sr = uint16_t((mcu.sr&~STATUS_C)|(carry ? STATUS_C : 0));
            mcu.pc += 2; return true;
        }
        case 0x31e8: mcu.r[5] = uint16_t((mcu.r[5]<<8)|(mcu.r[5]>>8)); nz(mcu.r[5]); mcu.pc += 2; return true;
        case 0x31ea: compare(mcu.r[5],255,true); mcu.pc += 2; return true;
        case 0x31ec: mcu.pc = (mcu.sr&STATUS_Z) ? 0x31ee : 0x31f0; return true;
        case 0x31ee: mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|254); nz(mcu.r[5],true); mcu.pc += 2; return true;
        case 0x31f0:
            if (mcu.r[1] >= 24) return false;
            MCU_Write(mcu,0xac42+mcu.r[1],uint8_t(mcu.r[5]));
            nz(mcu.r[5],true); mcu.pc += 4; return true;
        default: return false;
    }
}

// Select the next voice needing controller work, then clear the per-pass marks.
// The scan is interruptible; preserve every instruction boundary, including SCB.
inline bool TryStepVoiceScan(mcu_t& mcu)
{
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)) return false;
    auto nz = [&](uint16_t value, bool byte = false) {
        if (byte) value &= 255;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto twice = [&](unsigned reg) {
        const unsigned before = mcu.r[reg], result = before*2;
        mcu.r[reg] = uint16_t(result); nz(mcu.r[reg]);
        mcu.sr = uint16_t((mcu.sr&~3)|(result&65536 ? STATUS_C : 0)|((before^result)&32768 ? STATUS_V : 0));
        mcu.pc += 2;
    };
    auto voiceValid = [&] { return mcu.r[0] >= 0xacde && mcu.r[0] <= 0xc7a4 && (mcu.r[0]-0xacde)%0x12a == 0; };
    switch (mcu.pc) {
        case 0x5b0b: mcu.ep = 0; mcu.ex_ignore = 1; mcu.pc += 3; return true;
        case 0x5b0e: case 0x5b5f: mcu.r[1] = 23; nz(23); mcu.pc += 3; return true;
        case 0x5b11: case 0x5b62: mcu.r[2] = mcu.r[1]; nz(mcu.r[2]); mcu.pc += 2; return true;
        case 0x5b13: case 0x5b42: case 0x5b64: twice(2); return true;
        case 0x5b54: twice(0); return true;
        case 0x5b15: case 0x5b44: case 0x5b56: case 0x5b66: {
            const unsigned index = mcu.r[mcu.pc == 0x5b56 ? 0 : 2];
            if (index > 46 || (index&1)) return false;
            const unsigned dest = mcu.pc == 0x5b44 ? 2 : 0;
            mcu.r[dest] = ReadWord(mcu,0x676a+index); nz(mcu.r[dest]); mcu.pc += 4; return true;
        }
        case 0x5b19: {
            if (!voiceValid()) return false;
            const unsigned value = ReadWord(mcu,mcu.r[0]), result = uint16_t(value-18);
            mcu.sr = uint16_t((mcu.sr&~15)|(result&32768 ? STATUS_N : 0)|(!result ? STATUS_Z : 0)
                |(value < 18 ? STATUS_C : 0)|((value^18)&(value^result)&32768 ? STATUS_V : 0));
            mcu.pc += 4; return true;
        }
        case 0x5b1d: mcu.pc = (mcu.sr&STATUS_C) ? 0x5b1f : 0x5b5c; return true;
        case 0x5b1f:
            if (!voiceValid()) return false;
            mcu.sr &= ~STATUS_C; nz(MCU_Read(mcu,mcu.r[0]-26),true); mcu.pc += 3; return true;
        case 0x5b22: mcu.pc = (mcu.sr&STATUS_Z) ? 0x5b24 : 0x5b5c; return true;
        case 0x5b24:
            MCU_ControlRegisterWrite(mcu,0,MCU_Operand_Size::WORD,mcu.sr|0x700);
            mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x5b28: MCU_Write16(mcu,0xcb48,mcu.r[1]); nz(mcu.r[1]); mcu.pc += 4; return true;
        case 0x5b2c: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x5b2e: case 0x5b35: {
            if (mcu.r[1] >= 24) return false;
            const unsigned value = MCU_Read(mcu,(mcu.pc == 0x5b2e ? 0xcac4 : 0xcadc)+mcu.r[1]);
            const unsigned result = (value-255)&255;
            mcu.sr = uint16_t((mcu.sr&~15)|(result&128 ? STATUS_N : 0)|(!result ? STATUS_Z : 0)
                |(value < 255 ? STATUS_C : 0)|((value^255)&(value^result)&128 ? STATUS_V : 0));
            mcu.pc += 5; return true;
        }
        case 0x5b33: mcu.pc = (mcu.sr&STATUS_Z) ? 0x5b35 : 0x5b4a; return true;
        case 0x5b3a: mcu.pc = (mcu.sr&STATUS_Z) ? 0x5b73 : 0x5b3c; return true;
        case 0x5b3c: case 0x5b4a:
            if (mcu.r[1] >= 24) return false;
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|MCU_Read(mcu,(mcu.pc == 0x5b3c ? 0xcadc : 0xcac4)+mcu.r[1]));
            nz(mcu.r[3],true); mcu.pc += 4; return true;
        case 0x5b40: mcu.r[2] = mcu.r[3]; nz(mcu.r[2]); mcu.pc += 2; return true;
        case 0x5b48: case 0x5b5a: mcu.pc = 0x5bb4; return true;
        case 0x5b4e: { const auto value = mcu.r[1]; mcu.r[1] = mcu.r[3]; mcu.r[3] = value; mcu.pc += 2; return true; }
        case 0x5b50: mcu.r[2] = mcu.r[0]; nz(mcu.r[2]); mcu.pc += 2; return true;
        case 0x5b52: mcu.r[0] = mcu.r[1]; nz(mcu.r[0]); mcu.pc += 2; return true;
        case 0x5b5c: case 0x5b6d:
            --mcu.r[1]; mcu.pc = mcu.r[1] == 0xffff ? uint16_t(mcu.pc+3) : mcu.pc == 0x5b5c ? 0x5b11 : 0x5b62; return true;
        case 0x5b6a:
            if (!voiceValid()) return false;
            MCU_Write(mcu,mcu.r[0]-26,0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 3; return true;
        case 0x5b70: mcu.pc = 0x5942; return true;
        case 0x5b73: case 0x5b89: case 0x5b9c: case 0x5bb4: case 0x5bdb: case 0x5bf9:
            MCU_ControlRegisterWrite(mcu,0,MCU_Operand_Size::WORD,mcu.sr&0xf8ff);
            mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x5b7e: case 0x5b91: case 0x5bc6: case 0x5be3:
            MCU_ControlRegisterWrite(mcu,0,MCU_Operand_Size::WORD,mcu.sr|0x700);
            mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x5b8d: case 0x5b8e: case 0x5b8f: case 0x5b90:
        case 0x5bdf: case 0x5be0: case 0x5be1: case 0x5be2:
            ++mcu.pc; return true;
        case 0x5b77: case 0x5bb8:
            MCU_Write16(mcu,0xcb4a,mcu.r[0]); nz(mcu.r[0]); mcu.pc += 4; return true;
        case 0x5bbf:
            MCU_Write16(mcu,0xcb4c,mcu.r[2]); nz(mcu.r[2]); mcu.pc += 4; return true;
        case 0x5b7b: case 0x5bbc:
            if (!voiceValid()) return false;
            MCU_Write16(mcu,mcu.r[0]-2,mcu.r[1]); nz(mcu.r[1]); mcu.pc += 3; return true;
        case 0x5bc3:
            if (mcu.r[2] < 0xacde || mcu.r[2] > 0xc7a4 || (mcu.r[2]-0xacde)%0x12a) return false;
            MCU_Write16(mcu,mcu.r[2]-2,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x5b82: case 0x5b95: case 0x5ba0: case 0x5ba7: case 0x5bca:
        case 0x5be7: case 0x5bfd: case 0x5c0b:
            mcu.r[0] = ReadWord(mcu,0xcb4a); nz(mcu.r[0]); mcu.pc += 4; return true;
        case 0x5bd0: case 0x5bee: case 0x5c04: case 0x5c12:
            mcu.r[0] = ReadWord(mcu,0xcb4c); nz(mcu.r[0]); mcu.pc += 4; return true;
        case 0x5bd4: case 0x5bf2:
            mcu.r[2] = ReadWord(mcu,0xcb4a); nz(mcu.r[2]); mcu.pc += 4; return true;
        case 0x5bae: case 0x5c19:
            mcu.r[1] = ReadWord(mcu,0xcb48); nz(mcu.r[1]); mcu.pc += 4; return true;
        case 0x5bb2: case 0x5c1d: mcu.pc = 0x5b5c; return true;
        case 0x5b86: case 0x5b99: case 0x5ba4: case 0x5bab: case 0x5bce:
        case 0x5bd8: case 0x5beb: case 0x5bf6: case 0x5c01: case 0x5c08:
        case 0x5c0f: case 0x5c16: {
            // Only SRAM stacks are handled here. Other stacks retain the decoder path.
            if (mcu.tp || (mcu.r[7]&1) || mcu.r[7] < 0x8002 || mcu.r[7] > 0xe000) return false;
            const uint16_t target = mcu.pc == 0x5b86 || mcu.pc == 0x5bce ? 0x5c20
                : mcu.pc == 0x5b99 || mcu.pc == 0x5beb ? 0x3985
                : mcu.pc == 0x5ba4 || mcu.pc == 0x5c01 || mcu.pc == 0x5c08 ? 0x3188
                : mcu.pc == 0x5bd8 ? 0x5ff5 : mcu.pc == 0x5bf6 ? 0x3d44 : 0x5855;
            mcu.pc += mcu.pc == 0x5bce ? 2 : 3;
            MCU_PushStack(mcu,mcu.pc); mcu.pc = target; return true;
        }
        default: return false;
    }
}

// Validate/relink a shared voice before entering its modulation calculation.
inline bool TryStepVoiceLink(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](uint16_t value, bool byte = false) {
        if (byte) value &= 255;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto compare = [&](unsigned value, unsigned rhs, bool byte = false) {
        const unsigned mask = byte ? 255 : 65535, sign = byte ? 128 : 32768;
        value &= mask; rhs &= mask; const unsigned result = (value-rhs)&mask;
        mcu.sr = uint16_t((mcu.sr&~15)|(result&sign ? STATUS_N : 0)|(!result ? STATUS_Z : 0)
            |(value < rhs ? STATUS_C : 0)|((value^rhs)&(value^result)&sign ? STATUS_V : 0));
    };
    auto tableIndex = [](unsigned value) { return value <= 46 && !(value&1); };
    auto linkedVoice = [&] { return mcu.r[2] >= 0xacde && mcu.r[2] <= 0xc7a4 && (mcu.r[2]-0xacde)%0x12a == 0; };
    switch (mcu.pc) {
        case 0x3985: mcu.sr &= ~STATUS_C; nz(MCU_Read(mcu,voice-115),true); mcu.pc += 3; return true;
        case 0x3988: mcu.pc = (mcu.sr&STATUS_Z) ? 0x39f6 : 0x398a; return true;
        case 0x398a: case 0x39bb: mcu.r[1] = ReadWord(mcu,voice-2); nz(mcu.r[1]); mcu.pc += 3; return true;
        case 0x398d: case 0x39c0: case 0x39d2: {
            const unsigned reg = mcu.pc == 0x39d2 ? 3 : 1, before = mcu.r[reg], result = before*2;
            mcu.r[reg] = uint16_t(result); nz(mcu.r[reg]);
            mcu.sr = uint16_t((mcu.sr&~3)|(result&65536 ? STATUS_C : 0)|((before^result)&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x398f:
            if (!tableIndex(mcu.r[1])) return false;
            mcu.r[2] = ReadWord(mcu,0xc84e + mcu.r[1]); nz(mcu.r[2]); mcu.pc += 4; return true;
        case 0x3993: case 0x3997: {
            const unsigned reg = mcu.pc == 0x3993 ? 3 : 4, offset = mcu.pc == 0x3993 ? 155 : 152;
            mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|MCU_Read(mcu,voice+offset));
            nz(mcu.r[reg],true); mcu.pc += 4; return true;
        }
        case 0x399b: mcu.r[5] = ReadWord(mcu,voice+156); nz(mcu.r[5]); mcu.pc += 4; return true;
        case 0x399f:
            if (!linkedVoice()) return false;
            compare(ReadWord(mcu,mcu.r[2]),12); mcu.pc += 5; return true;
        case 0x39a4: mcu.pc = (mcu.sr&(STATUS_C|STATUS_Z)) ? 0x39a6 : 0x39bb; return true;
        case 0x39a6: case 0x39ac: case 0x39b2:
            if (!linkedVoice()) return false;
            if (mcu.pc == 0x39b2) compare(mcu.r[5],ReadWord(mcu,mcu.r[2]+156));
            else compare(mcu.r[mcu.pc == 0x39a6 ? 3 : 4],MCU_Read(mcu,mcu.r[2]+(mcu.pc == 0x39a6 ? 155 : 152)),true);
            mcu.pc += 4; return true;
        case 0x39aa: case 0x39b0: case 0x39b6:
            mcu.pc = (mcu.sr&STATUS_Z) ? uint16_t(mcu.pc+2) : 0x39bb; return true;
        case 0x39b8: mcu.pc = 0x3d44; return true;
        case 0x39be: mcu.r[6] = mcu.r[1]; nz(mcu.r[6]); mcu.pc += 2; return true;
        case 0x39c2:
            if (!tableIndex(mcu.r[1])) return false;
            MCU_Write16(mcu,0xc84e + mcu.r[1],0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 4; return true;
        case 0x39c6: MCU_Write(mcu,voice-115,0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 3; return true;
        case 0x39c9: mcu.r[1] = 23; nz(23); mcu.pc += 3; return true;
        case 0x39cc: compare(mcu.r[6],mcu.r[1]); mcu.pc += 2; return true;
        case 0x39ce: mcu.pc = (mcu.sr&STATUS_Z) ? 0x39de : 0x39d0; return true;
        case 0x39d0: mcu.r[3] = mcu.r[1]; nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x39d4:
            if (!tableIndex(mcu.r[3])) return false;
            compare(mcu.r[2],ReadWord(mcu,0xc84e + mcu.r[3])); mcu.pc += 4; return true;
        case 0x39d8: mcu.pc = (mcu.sr&STATUS_Z) ? 0x39da : 0x39de; return true;
        case 0x39da:
            if (!tableIndex(mcu.r[3])) return false;
            MCU_Write16(mcu,0xc84e + mcu.r[3],mcu.r[0]); nz(mcu.r[0]); mcu.pc += 4; return true;
        case 0x39de: --mcu.r[1]; mcu.pc = mcu.r[1] == 0xffff ? 0x39e1 : 0x39cc; return true;
        case 0x39e1: mcu.r[6] = ReadWord(mcu,voice); nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x39e4: case 0x39ec:
            MCU_ControlRegisterWrite(mcu,0,MCU_Operand_Size::WORD,mcu.pc == 0x39e4 ? mcu.sr&0xf8ff : mcu.sr|0x700);
            mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x39e8: case 0x39e9: case 0x39ea: case 0x39eb: ++mcu.pc; return true;
        case 0x39f0: compare(mcu.r[6],ReadWord(mcu,voice)); mcu.pc += 3; return true;
        case 0x39f3: mcu.pc = (mcu.sr&STATUS_Z) ? 0x39f6 : 0x39f5; return true;
        case 0x39f5:
            if ((mcu.r[7]&1) || mcu.r[7] < 0x8000 || mcu.r[7] > 0xdffe) return false;
            ++mcu.pc; mcu.pc = MCU_PopStack(mcu); return true;
        default: return false;
    }
}

inline bool TryStepVoiceParameterBias(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = true) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto byteMove = [&](unsigned reg, unsigned value) {
        mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|(value&255)); nz(value);
    };
    auto arithmetic = [&](unsigned reg, unsigned a, unsigned b, bool sub, bool byte = true) {
        const unsigned mask = byte ? 255 : 65535, sign = byte ? 128 : 32768;
        a &= mask; b &= mask;
        const unsigned wide = sub ? a-b : a+b, value = wide&mask;
        mcu.r[reg] = uint16_t((byte ? mcu.r[reg]&0xff00 : 0)|value);
        mcu.sr = uint16_t((mcu.sr&~15)|(value&sign ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
            |(wide&(mask+1) ? STATUS_C : 0)
            |((sub ? (a^b)&(a^value) : ~(a^b)&(a^value))&sign ? STATUS_V : 0));
    };
    switch (mcu.pc) {
        case 0x39f6: case 0x3a17: {
            const unsigned reg = mcu.pc == 0x39f6 ? 3 : 2;
            mcu.r[reg] = ReadWord(mcu,voice+46); nz(mcu.r[reg],false); mcu.pc += 3; return true;
        }
        case 0x39f9: case 0x3a1a: {
            const unsigned address = uint16_t(mcu.r[mcu.pc == 0x39f9 ? 3 : 2]+(mcu.pc == 0x39f9 ? 16 : 17));
            if (address < 0x8000 || address > 0xdfff) return false;
            byteMove(mcu.pc == 0x39f9 ? 4 : 3,MCU_Read(mcu,address)); mcu.pc += 3; return true;
        }
        case 0x39fc: byteMove(3,MCU_Read(mcu,voice-25)); mcu.pc += 3; return true;
        case 0x39ff: arithmetic(4,mcu.r[4],64,true); mcu.pc += 3; return true;
        case 0x3a02: mcu.pc = (mcu.sr&STATUS_N) ? 0x3a04 : 0x3a0c; return true;
        case 0x3a04: case 0x3a0c: arithmetic(3,mcu.r[3],mcu.r[4],false); mcu.pc += 2; return true;
        case 0x3a06: case 0x3a0e: mcu.pc = (mcu.sr&STATUS_N) ? uint16_t(mcu.pc+2) : 0x3a12; return true;
        case 0x3a08: case 0x3a15: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x3a0a: mcu.pc = 0x3a12; return true;
        case 0x3a10: byteMove(3,127); mcu.pc += 2; return true;
        case 0x3a12: MCU_Write(mcu,voice-116,uint8_t(mcu.r[3])); nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x3a1d: mcu.r[2] = ReadWord(mcu,voice+168); nz(mcu.r[2],false); mcu.pc += 4; return true;
        case 0x3a21: mcu.sr &= ~STATUS_C; nz(mcu.r[2]); mcu.pc += 2; return true;
        case 0x3a23: mcu.pc = (mcu.sr&STATUS_N) ? 0x3a25 : 0x3a2f; return true;
        case 0x3a25: byteMove(2,mcu.r[2]&127); mcu.pc += 3; return true;
        case 0x3a28: case 0x3a2f: arithmetic(3,mcu.r[3],64,true); mcu.pc += 3; return true;
        case 0x3a2b: mcu.pc = (mcu.sr&STATUS_C) ? 0x3a2d : 0x3a56; return true;
        case 0x3a2d: mcu.pc = 0x3a4a; return true;
        case 0x3a32: mcu.pc = (mcu.sr&STATUS_C) ? 0x3a34 : 0x3a40; return true;
        case 0x3a34: case 0x3a4a: arithmetic(3,0,mcu.r[3],true); mcu.pc += 2; return true;
        case 0x3a36: case 0x3a40: case 0x3a4c: case 0x3a56: arithmetic(3,mcu.r[3],mcu.r[3],false); mcu.pc += 2; return true;
        case 0x3a38: case 0x3a58: arithmetic(2,mcu.r[2],mcu.r[3],true); mcu.pc += 2; return true;
        case 0x3a42: case 0x3a4e: arithmetic(2,mcu.r[2],mcu.r[3],false); mcu.pc += 2; return true;
        case 0x3a3a: case 0x3a44: mcu.pc = (mcu.sr&STATUS_N) ? uint16_t(mcu.pc+2) : 0x3a68; return true;
        case 0x3a50: case 0x3a5a: mcu.pc = (mcu.sr&STATUS_N) ? uint16_t(mcu.pc+2) : 0x3a5e; return true;
        case 0x3a3c: case 0x3a5c: mcu.r[2] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x3a46: case 0x3a52: byteMove(2,127); mcu.pc += 2; return true;
        case 0x3a3e: case 0x3a48: mcu.pc = 0x3a68; return true;
        case 0x3a54: mcu.pc = 0x3a5e; return true;
        case 0x3a5e: case 0x3a68: arithmetic(2,mcu.r[2],mcu.r[2],false,false); mcu.pc += 2; return true;
        case 0x3a60: case 0x3a6a:
            if (mcu.r[2] > 254 || (mcu.r[2]&1)) return false;
            mcu.r[2] = ReadWord(mcu,0x7312+mcu.r[2]); nz(mcu.r[2],false); mcu.pc += 4; return true;
        case 0x3a64: arithmetic(2,0,mcu.r[2],true,false); mcu.pc += 2; return true;
        case 0x3a66: mcu.pc = 0x3a6e; return true;
        case 0x3a6e: MCU_Write16(mcu,voice-124,mcu.r[2]); nz(mcu.r[2],false); mcu.pc += 3; return true;
        case 0x3a71: mcu.r[1] = mcu.r[0]; nz(mcu.r[1],false); mcu.pc += 2; return true;
        case 0x3a73: arithmetic(1,mcu.r[1],0xff80,false,false); mcu.pc += 4; return true;
        case 0x3a77: mcu.pc = 0x3b2c; return true;
        default: return false;
    }
}

// Copy an already calculated second modulation state from a matching voice.
inline bool TryStepSharedModulationCopy(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0], source = mcu.r[2];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    if (mcu.pc == 0x3aea) {
        if ((mcu.r[7]&1) || mcu.r[7] < 0x8000 || mcu.r[7] > 0xdffe) return false;
        ++mcu.pc; mcu.pc = MCU_PopStack(mcu); return true;
    }
    if (mcu.pc < 0x3aae || mcu.pc > 0x3ae7) return false;
    const unsigned relative = mcu.pc-0x3aae;
    if (relative%3) return false;
    constexpr unsigned distances[]{88,86,84,80,72,70,68,66,64,62};
    const unsigned index = relative/6;
    if (relative%6 == 0) {
        if (source < 0xacde || source > 0xc7a4 || (source-0xacde)%0x12a) return false;
        mcu.r[6] = ReadWord(mcu,source-distances[index]);
    } else MCU_Write16(mcu,voice-distances[index],mcu.r[6]);
    mcu.sr = uint16_t((mcu.sr&~14)|(mcu.r[6]&32768 ? STATUS_N : 0)|(!mcu.r[6] ? STATUS_Z : 0));
    mcu.pc += 3; return true;
}

inline bool TryStepSecondVoiceLink(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](uint16_t value, bool byte = false) {
        if (byte) value &= 255;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto compare = [&](unsigned value, unsigned rhs, bool byte = false) {
        const unsigned mask = byte ? 255 : 65535, sign = byte ? 128 : 32768;
        value &= mask; rhs &= mask; const unsigned result = (value-rhs)&mask;
        mcu.sr = uint16_t((mcu.sr&~15)|(result&sign ? STATUS_N : 0)|(!result ? STATUS_Z : 0)
            |(value < rhs ? STATUS_C : 0)|((value^rhs)&(value^result)&sign ? STATUS_V : 0));
    };
    auto tableIndex = [](unsigned value) { return value <= 46 && !(value&1); };
    auto linkedVoice = [&] { return mcu.r[2] >= 0xacde && mcu.r[2] <= 0xc7a4 && (mcu.r[2]-0xacde)%0x12a == 0; };
    switch (mcu.pc) {
        case 0x3a7a: mcu.sr &= ~STATUS_C; nz(MCU_Read(mcu,voice-81),true); mcu.pc += 3; return true;
        case 0x3a7d: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3b26 : 0x3a80; return true;
        case 0x3a80: case 0x3aeb: mcu.r[1] = ReadWord(mcu,voice-2); nz(mcu.r[1]); mcu.pc += 3; return true;
        case 0x3a83: case 0x3af0: case 0x3b02: {
            const unsigned reg = mcu.pc == 0x3b02 ? 3 : 1, before = mcu.r[reg], result = before*2;
            mcu.r[reg] = uint16_t(result); nz(mcu.r[reg]);
            mcu.sr = uint16_t((mcu.sr&~3)|(result&65536 ? STATUS_C : 0)|((before^result)&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x3a85:
            if (!tableIndex(mcu.r[1])) return false;
            mcu.r[2] = ReadWord(mcu,0xc87e + mcu.r[1]); nz(mcu.r[2]); mcu.pc += 4; return true;
        case 0x3a89: case 0x3a8d: {
            const unsigned reg = mcu.pc == 0x3a89 ? 3 : 4, offset = mcu.pc == 0x3a89 ? 155 : 153;
            mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|MCU_Read(mcu,voice+offset));
            nz(mcu.r[reg],true); mcu.pc += 4; return true;
        }
        case 0x3a91: mcu.r[5] = ReadWord(mcu,voice+158); nz(mcu.r[5]); mcu.pc += 4; return true;
        case 0x3a95:
            if (!linkedVoice()) return false;
            compare(ReadWord(mcu,mcu.r[2]),12); mcu.pc += 5; return true;
        case 0x3a9a: mcu.pc = (mcu.sr&(STATUS_C|STATUS_Z)) ? 0x3a9c : 0x3aeb; return true;
        case 0x3a9c: case 0x3aa2: case 0x3aa8:
            if (!linkedVoice()) return false;
            if (mcu.pc == 0x3aa8) compare(mcu.r[5],ReadWord(mcu,mcu.r[2]+158));
            else compare(mcu.r[mcu.pc == 0x3a9c ? 3 : 4],MCU_Read(mcu,mcu.r[2]+(mcu.pc == 0x3a9c ? 155 : 153)),true);
            mcu.pc += 4; return true;
        case 0x3aa0: case 0x3aa6: case 0x3aac:
            mcu.pc = (mcu.sr&STATUS_Z) ? uint16_t(mcu.pc+2) : 0x3aeb; return true;
        case 0x3aee: mcu.r[6] = mcu.r[1]; nz(mcu.r[6]); mcu.pc += 2; return true;
        case 0x3af2:
            if (!tableIndex(mcu.r[1])) return false;
            MCU_Write16(mcu,0xc87e + mcu.r[1],0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 4; return true;
        case 0x3af6: MCU_Write(mcu,voice-81,0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 3; return true;
        case 0x3af9: mcu.r[1] = 23; nz(23); mcu.pc += 3; return true;
        case 0x3afc: compare(mcu.r[6],mcu.r[1]); mcu.pc += 2; return true;
        case 0x3afe: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3b0e : 0x3b00; return true;
        case 0x3b00: mcu.r[3] = mcu.r[1]; nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x3b04:
            if (!tableIndex(mcu.r[3])) return false;
            compare(mcu.r[2],ReadWord(mcu,0xc87e + mcu.r[3])); mcu.pc += 4; return true;
        case 0x3b08: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3b0a : 0x3b0e; return true;
        case 0x3b0a:
            if (!tableIndex(mcu.r[3])) return false;
            MCU_Write16(mcu,0xc87e + mcu.r[3],mcu.r[0]); nz(mcu.r[0]); mcu.pc += 4; return true;
        case 0x3b0e: --mcu.r[1]; mcu.pc = mcu.r[1] == 0xffff ? 0x3b11 : 0x3afc; return true;
        case 0x3b11: mcu.r[6] = ReadWord(mcu,voice); nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x3b14: case 0x3b1c:
            MCU_ControlRegisterWrite(mcu,0,MCU_Operand_Size::WORD,mcu.pc == 0x3b14 ? mcu.sr&0xf8ff : mcu.sr|0x700);
            mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x3b18: case 0x3b19: case 0x3b1a: case 0x3b1b: ++mcu.pc; return true;
        case 0x3b20: compare(mcu.r[6],ReadWord(mcu,voice)); mcu.pc += 3; return true;
        case 0x3b23: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3b26 : 0x3b25; return true;
        case 0x3b25:
            if ((mcu.r[7]&1) || mcu.r[7] < 0x8000 || mcu.r[7] > 0xdffe) return false;
            ++mcu.pc; mcu.pc = MCU_PopStack(mcu); return true;
        default: return false;
    }
}

// Interruptible delay/attack phase of the two modulation blocks.
inline bool TryStepLfoPhase(mcu_t& mcu)
{
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)) return false;
    const unsigned block = mcu.r[1];
    if (mcu.pc != 0x3b26 && mcu.pc != 0x3b28 && (block < 0x8000 || block > 0xdfe4 || (block&1))) return false;
    auto nz = [&](uint16_t value) {
        mcu.sr = uint16_t((mcu.sr&~14)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto add = [&](uint16_t a, uint16_t b, unsigned carry = 0) {
        const unsigned result = unsigned(a)+b+carry;
        const int signedResult = int(int16_t(a))+int(int16_t(b))+int(carry);
        mcu.sr = uint16_t((mcu.sr&~15)|(result&32768 ? STATUS_N : 0)|(!(result&65535) ? STATUS_Z : 0)
            |(result&65536 ? STATUS_C : 0)|(signedResult < -32768 || signedResult > 32767 ? STATUS_V : 0));
        return uint16_t(result);
    };
    auto compareMax = [&](uint16_t value) {
        const unsigned result = uint16_t(value-65535);
        mcu.sr = uint16_t((mcu.sr&~15)|(result&32768 ? STATUS_N : 0)|(!result ? STATUS_Z : 0)
            |(value != 65535 ? STATUS_C : 0)|(value == 32767 ? STATUS_V : 0));
    };
    switch (mcu.pc) {
        case 0x3b26: mcu.r[1] = mcu.r[0]; nz(mcu.r[1]); mcu.pc += 2; return true;
        case 0x3b28: mcu.r[1] = add(mcu.r[1],0xffa2); mcu.pc += 4; return true;
        case 0x3b2c: case 0x3b52:
            mcu.r[6] = ReadWord(mcu,block+(mcu.pc == 0x3b2c ? 24 : 26)); nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x3b2f: case 0x3b55: compareMax(mcu.r[6]); mcu.pc += 3; return true;
        case 0x3b48: compareMax(mcu.r[5]); mcu.pc += 3; return true;
        case 0x3b32: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3b52 : 0x3b34; return true;
        case 0x3b58: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3baa : 0x3b5a; return true;
        case 0x3b34: case 0x3b5a:
            mcu.r[4] = ReadWord(mcu,block+(mcu.pc == 0x3b34 ? 16 : 18)); nz(mcu.r[4]); mcu.pc += 3; return true;
        case 0x3b37: case 0x3b5d: case 0x3be1: {
            const uint32_t product = uint32_t(mcu.r[4])*ReadWord(mcu,0xac5a);
            mcu.r[4] = uint16_t(product>>16); mcu.r[5] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += 4; return true;
        }
        case 0x3b3b: case 0x3b61: mcu.r[5] = add(mcu.r[5],mcu.r[6]); mcu.pc += 2; return true;
        case 0x3b3d: case 0x3b63: {
            const bool previousZero = (mcu.sr&STATUS_Z) != 0;
            mcu.r[4] = add(mcu.r[4],0,(mcu.sr&STATUS_C) != 0);
            if (!previousZero) mcu.sr &= ~STATUS_Z;
            mcu.pc += 4; return true;
        }
        case 0x3b41: case 0x3b67: mcu.sr &= ~STATUS_C; nz(mcu.r[4]); mcu.pc += 2; return true;
        case 0x3b43: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3b45 : 0x3b4d; return true;
        case 0x3b69: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3b6e : 0x3b6b; return true;
        case 0x3b45: case 0x3b6e:
            MCU_Write16(mcu,block+(mcu.pc == 0x3b45 ? 24 : 26),mcu.r[5]); nz(mcu.r[5]); mcu.pc += 3; return true;
        case 0x3b4b: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3b4d : 0x3bbc; return true;
        case 0x3b4d: MCU_Write16(mcu,block+24,65535); nz(65535); mcu.pc += 5; return true;
        case 0x3b6b: mcu.r[5] = 65535; nz(65535); mcu.pc += 3; return true;
        case 0x3b71: case 0x3baa: mcu.r[2] = ReadWord(mcu,block); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3b74: mcu.pc = (mcu.sr&STATUS_N) ? 0x3b76 : 0x3b7e; return true;
        case 0x3b86: mcu.pc = (mcu.sr&STATUS_N) ? 0x3b88 : 0x3b90; return true;
        case 0x3b99: mcu.pc = (mcu.sr&STATUS_N) ? 0x3b9b : 0x3ba3; return true;
        case 0x3b76: case 0x3b7a: case 0x3b88: case 0x3b8c: case 0x3b9b: case 0x3b9f: {
            const unsigned previous = mcu.r[2]; mcu.r[2] = uint16_t(0u-previous);
            mcu.sr = uint16_t((mcu.sr&~15)|(mcu.r[2]&32768 ? STATUS_N : 0)|(!mcu.r[2] ? STATUS_Z : 0)
                |(previous ? STATUS_C : 0)|(previous == 32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x3b78: case 0x3b7e: case 0x3b8a: case 0x3b90: case 0x3b9d: case 0x3ba3: {
            const uint32_t product = uint32_t(mcu.r[2])*mcu.r[5];
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += 2; return true;
        }
        case 0x3b7c: mcu.pc = 0x3b80; return true;
        case 0x3b8e: mcu.pc = 0x3b92; return true;
        case 0x3ba1: mcu.pc = 0x3ba5; return true;
        case 0x3b80: case 0x3bad:
            MCU_Write16(mcu,block+6,mcu.r[2]); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3b92: case 0x3bb3:
            MCU_Write16(mcu,block+8,mcu.r[2]); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3ba5: case 0x3bb9:
            MCU_Write16(mcu,block+10,mcu.r[2]); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3b83: case 0x3bb0:
            mcu.r[2] = ReadWord(mcu,block+2); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3b95: case 0x3bb6:
            mcu.r[2] = ReadWord(mcu,block+4); nz(mcu.r[2]); mcu.pc += mcu.pc == 0x3b95 ? 4 : 3; return true;
        case 0x3ba8: mcu.pc = 0x3bbc; return true;
        case 0x3bbc: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x3bbe:
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|MCU_Read(mcu,block+12));
            mcu.sr = uint16_t((mcu.sr&~14)|(mcu.r[3]&128 ? STATUS_N : 0)|(!(mcu.r[3]&255) ? STATUS_Z : 0));
            mcu.pc += 3; return true;
        case 0x3bc1: mcu.r[3] = add(mcu.r[3],mcu.r[3]); mcu.pc += 2; return true;
        case 0x3bc3:
            if (mcu.r[3] > 510 || (mcu.r[3]&1)) return false;
            mcu.r[3] = ReadWord(mcu,0x7012+mcu.r[3]); nz(mcu.r[3]); mcu.pc += 4; return true;
        case 0x3bc7: mcu.r[4] = ReadWord(mcu,block+14); nz(mcu.r[4]); mcu.pc += 3; return true;
        case 0x3bca: mcu.pc = (mcu.sr&STATUS_N) ? 0x3bcc : 0x3bd7; return true;
        case 0x3bcc: case 0x3bd7: mcu.r[4] = add(mcu.r[4],mcu.r[3]); mcu.pc += 2; return true;
        case 0x3bce: case 0x3bd9: {
            const unsigned previous = mcu.r[4], result = uint16_t(previous-0x28f6);
            mcu.sr = uint16_t((mcu.sr&~15)|(result&32768 ? STATUS_N : 0)|(!result ? STATUS_Z : 0)
                |(previous < 0x28f6 ? STATUS_C : 0)|((previous^0x28f6)&(previous^result)&32768 ? STATUS_V : 0));
            mcu.pc += 3; return true;
        }
        case 0x3bd1: case 0x3bdc: mcu.pc = (mcu.sr&(STATUS_C|STATUS_Z)) ? 0x3be1 : uint16_t(mcu.pc+2); return true;
        case 0x3bd3: mcu.r[4] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x3bd5: mcu.pc = 0x3be1; return true;
        case 0x3bde: mcu.r[4] = 0x28f6; nz(mcu.r[4]); mcu.pc += 3; return true;
        case 0x3be5: mcu.r[2] = ReadWord(mcu,block+20); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3be8:
            if (mcu.r[2] > 10 || (mcu.r[2]&1)) return false;
            mcu.r[2] = ReadWord(mcu,0x74c4+mcu.r[2]); nz(mcu.r[2]); mcu.pc += 4; return true;
        case 0x3bec: mcu.pc = mcu.r[2]; return true;
        default: return false;
    }
}

inline bool TryStepLfoSine(mcu_t& mcu)
{
    const unsigned block = mcu.r[1];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || block < 0x8000 || block > 0xdfde || (block&1)) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto arithmetic = [&](unsigned a, unsigned b, bool sub, bool byte = false) {
        const unsigned mask = byte ? 255 : 65535, sign = byte ? 128 : 32768;
        a &= mask; b &= mask; const unsigned wide = sub ? a-b : a+b, result = wide&mask;
        mcu.sr = uint16_t((mcu.sr&~15)|(result&sign ? STATUS_N : 0)|(!result ? STATUS_Z : 0)
            |(wide&(mask+1) ? STATUS_C : 0)|((sub ? (a^b)&(a^result) : ~(a^b)&(a^result))&sign ? STATUS_V : 0));
        return uint16_t(result);
    };
    switch (mcu.pc) {
        case 0x3bee: case 0x3c31: case 0x3c48: case 0x3c5c: mcu.r[5] = arithmetic(mcu.r[5],ReadWord(mcu,block+22),false); mcu.pc += 3; return true;
        case 0x3bf1: case 0x3c34: case 0x3c4b: case 0x3c5f: MCU_Write16(mcu,block+22,mcu.r[5]); nz(mcu.r[5]); mcu.pc += 3; return true;
        case 0x3bf4: mcu.r[6] = mcu.r[5]; nz(mcu.r[6]); mcu.pc += 2; return true;
        case 0x3bf6: case 0x3c4e: case 0x3c64: mcu.r[5] = arithmetic(mcu.r[5],0x8000,true); mcu.pc += 4; return true;
        case 0x3bfa: mcu.pc = (mcu.sr&STATUS_C) ? 0x3bfc : 0x3bfe; return true;
        case 0x3bfc: case 0x3c6c: case 0x3c76: case 0x3c92: mcu.r[5] = arithmetic(0,mcu.r[5],true); mcu.pc += 2; return true;
        case 0x3bfe: mcu.r[2] = mcu.r[5]; nz(mcu.r[2]); mcu.pc += 2; return true;
        case 0x3c00: mcu.r[2] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x3c02: mcu.r[2] = uint16_t((mcu.r[2]<<8)|(mcu.r[2]>>8)); nz(mcu.r[2]); mcu.pc += 2; return true;
        case 0x3c04: case 0x3c62: mcu.r[4] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x3c06: case 0x3c0c: {
            if (mcu.r[2] > 255) return false;
            const unsigned reg = mcu.pc == 0x3c06 ? 4 : 2, value = MCU_Read(mcu,0x7412+mcu.r[2]);
            mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|value); nz(value,true); mcu.pc += 4; return true;
        }
        case 0x3c0a: mcu.r[2] = arithmetic(mcu.r[2],1,false); mcu.pc += 2; return true;
        case 0x3c10: case 0x3c14: {
            const auto value = arithmetic(mcu.pc == 0x3c10 ? mcu.r[2] : 0,mcu.pc == 0x3c10 ? mcu.r[4] : mcu.r[2],true,true);
            mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|value); mcu.pc += 2; return true;
        }
        case 0x3c12: mcu.pc = (mcu.sr&STATUS_C) ? 0x3c14 : 0x3c1e; return true;
        case 0x3c16: case 0x3c1e:
            mcu.r[2] = uint16_t((mcu.r[2]&255)*(mcu.r[5]&255)); mcu.sr &= ~STATUS_C; nz(mcu.r[2]); mcu.pc += 2; return true;
        case 0x3c18: case 0x3c20: mcu.r[4] = uint16_t((mcu.r[4]<<8)|(mcu.r[4]>>8)); nz(mcu.r[4]); mcu.pc += 2; return true;
        case 0x3c1a: mcu.r[4] = arithmetic(mcu.r[4],mcu.r[2],true); mcu.pc += 2; return true;
        case 0x3c1c: mcu.pc = 0x3c24; return true;
        case 0x3c22: mcu.r[4] = arithmetic(mcu.r[4],mcu.r[2],false); mcu.pc += 2; return true;
        case 0x3c24: {
            const bool carry = (mcu.r[4]&1) != 0; mcu.r[4] >>= 1; nz(mcu.r[4]);
            mcu.sr = uint16_t((mcu.sr&~STATUS_C)|(carry ? STATUS_C : 0)); mcu.pc += 2; return true;
        }
        case 0x3c26: arithmetic(mcu.r[6],0x8000,true); mcu.pc += 3; return true;
        case 0x3c29: mcu.pc = (mcu.sr&(STATUS_C|STATUS_Z)) ? 0x3c2d : 0x3c2b; return true;
        case 0x3c2b: mcu.r[4] = arithmetic(0,mcu.r[4],true); mcu.pc += 2; return true;
        case 0x3c2d: case 0x3ca4: MCU_Write16(mcu,block+32,mcu.r[4]); nz(mcu.r[4]); mcu.pc += 3; return true;
        case 0x3c37: arithmetic(mcu.r[5],0x8000,true); mcu.pc += 3; return true;
        case 0x3c3a: mcu.pc = (mcu.sr&STATUS_C) ? 0x3c41 : 0x3c3c; return true;
        case 0x3c3c: mcu.r[2] = 0x8001; nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3c41: mcu.r[2] = 0x7fff; nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3c3f: mcu.pc = 0x3c44; return true;
        case 0x3c44: MCU_Write16(mcu,block+32,mcu.r[2]); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3c52: mcu.pc = (mcu.sr&STATUS_C) ? 0x3c54 : 0x3c58; return true;
        case 0x3c54: case 0x3c58: case 0x3ca8: MCU_Write16(mcu,block+32,mcu.r[5]); nz(mcu.r[5]); mcu.pc += 3; return true;
        case 0x3c68: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3ca8 : 0x3c6a; return true;
        case 0x3c6a: mcu.pc = (mcu.sr&STATUS_C) ? 0x3c6c : 0x3c8a; return true;
        case 0x3c6e: case 0x3c8a: mcu.r[5] = arithmetic(mcu.r[5],0x4000,true); mcu.pc += 4; return true;
        case 0x3c72: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3c83 : 0x3c74; return true;
        case 0x3c8e: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3c9f : 0x3c90; return true;
        case 0x3c74: mcu.pc = (mcu.sr&STATUS_C) ? 0x3c76 : 0x3c78; return true;
        case 0x3c90: mcu.pc = (mcu.sr&STATUS_C) ? 0x3c92 : 0x3c94; return true;
        case 0x3c78: case 0x3c94: mcu.r[5] = arithmetic(mcu.r[5],mcu.r[5],false); mcu.pc += 2; return true;
        case 0x3c7a: case 0x3c96: mcu.r[4] = mcu.r[5]; nz(mcu.r[4]); mcu.pc += 2; return true;
        case 0x3c7c: case 0x3c98: mcu.r[3] = 0x8000; nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x3c7f: case 0x3c9b: mcu.r[3] = arithmetic(mcu.r[3],mcu.r[4],true); mcu.pc += 2; return true;
        case 0x3c81: mcu.pc = 0x3c86; return true;
        case 0x3c9d: mcu.pc = 0x3ca2; return true;
        case 0x3c83: case 0x3c9f: mcu.r[3] = 0x7fff; nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x3c86: MCU_Write16(mcu,block+32,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x3ca2: mcu.r[3] = arithmetic(0,mcu.r[3],true); mcu.pc += 2; return true;
        case 0x3c30: case 0x3c47: case 0x3c57: case 0x3c5b: case 0x3c89: case 0x3ca7: case 0x3cab:
            if ((mcu.r[7]&1) || mcu.r[7] < 0x8000 || mcu.r[7] > 0xdffe) return false;
            ++mcu.pc; mcu.pc = MCU_PopStack(mcu); return true;
        default: return false;
    }
}

// Sample-and-hold LFO: update the held PCM-derived value only on phase overflow.
inline bool TryStepLfoSampleHold(mcu_t& mcu)
{
    const unsigned block = mcu.r[1];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || block < 0x8000 || block > 0xdfde || (block&1)) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto add = [&](uint16_t a, uint16_t b, unsigned carry = 0) {
        const unsigned result = unsigned(a)+b+carry;
        const int signedResult = int(int16_t(a))+int(int16_t(b))+int(carry);
        mcu.sr = uint16_t((mcu.sr&~15)|(result&32768 ? STATUS_N : 0)|(!(result&65535) ? STATUS_Z : 0)
            |(result&65536 ? STATUS_C : 0)|(signedResult < -32768 || signedResult > 32767 ? STATUS_V : 0));
        return uint16_t(result);
    };
    switch (mcu.pc) {
        case 0x3cd0: case 0x3cd3: mcu.r[6] = mcu.pc == 0x3cd0 ? 0x900 : 0x50; nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x3cac: case 0x3cd6: mcu.r[5] = add(mcu.r[5],mcu.r[5]); mcu.pc += 2; return true;
        case 0x3cae: case 0x3cb3: case 0x3cd8: case 0x3cdd: {
            const bool previousZero = (mcu.sr&STATUS_Z) != 0;
            const bool doubling = mcu.pc == 0x3cae || mcu.pc == 0x3cd8;
            mcu.r[4] = add(mcu.r[4],doubling ? mcu.r[4] : 0,(mcu.sr&STATUS_C) != 0);
            if (!previousZero) mcu.sr &= ~STATUS_Z;
            mcu.pc += doubling ? 2 : 4; return true;
        }
        case 0x3cb0: case 0x3cda: mcu.r[5] = add(mcu.r[5],ReadWord(mcu,block+22)); mcu.pc += 3; return true;
        case 0x3cb7: case 0x3ce1: mcu.sr &= ~STATUS_C; nz(mcu.r[4]); mcu.pc += 2; return true;
        case 0x3cb9: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3cc6 : 0x3cbb; return true;
        case 0x3ce3: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3cf0 : 0x3ce5; return true;
        case 0x3cbb: case 0x3ce5:
            if (mcu.br != 0xe0) return false;
            mcu.pc += 4; MCU_Write(mcu,0xe03e,30); nz(30,true); return true;
        case 0x3cbf: case 0x3ce9:
            if (mcu.br != 0xe0) return false;
            mcu.pc += 2; mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|MCU_Read(mcu,0xe034)); nz(mcu.r[4],true); return true;
        case 0x3cc1: case 0x3ceb:
            if (mcu.br != 0xe0) return false;
            mcu.pc += 2; mcu.r[4] = MCU_Read16(mcu,0xe03a); nz(mcu.r[4]); return true;
        case 0x3cc3: case 0x3ced: MCU_Write16(mcu,block+28,mcu.r[4]); nz(mcu.r[4]); mcu.pc += 3; return true;
        case 0x3cc6: case 0x3cf0: MCU_Write16(mcu,block+22,mcu.r[5]); nz(mcu.r[5]); mcu.pc += 3; return true;
        case 0x3cc9: mcu.r[4] = ReadWord(mcu,block+28); nz(mcu.r[4]); mcu.pc += 3; return true;
        case 0x3cf3: mcu.r[4] = ReadWord(mcu,block+30); nz(mcu.r[4]); mcu.pc += 3; return true;
        case 0x3cf6: mcu.r[5] = ReadWord(mcu,block+28); nz(mcu.r[5]); mcu.pc += 3; return true;
        case 0x3cf9: case 0x3d03: case 0x3d0d: case 0x3cff: {
            const bool store = mcu.pc == 0x3cff;
            const unsigned a = mcu.r[store ? 4 : 5], b = mcu.r[store ? 6 : 4], result = uint16_t(a-b);
            mcu.sr = uint16_t((mcu.sr&~15)|(result&32768 ? STATUS_N : 0)|(!result ? STATUS_Z : 0)
                |(a < b ? STATUS_C : 0)|((a^b)&(a^result)&32768 ? STATUS_V : 0));
            if (store) mcu.r[4] = uint16_t(result);
            mcu.pc += 2; return true;
        }
        case 0x3cfb: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3d13 : 0x3cfd; return true;
        case 0x3cfd: case 0x3d0f: {
            const bool ge = bool(mcu.sr&STATUS_N) == bool(mcu.sr&STATUS_V);
            mcu.pc = ge ? (mcu.pc == 0x3cfd ? 0x3d09 : 0x3d13) : uint16_t(mcu.pc+2); return true;
        }
        case 0x3d01: case 0x3d0b: mcu.pc = (mcu.sr&STATUS_V) ? 0x3d11 : uint16_t(mcu.pc+2); return true;
        case 0x3d05:
            mcu.pc = !(mcu.sr&STATUS_Z) && bool(mcu.sr&STATUS_N) == bool(mcu.sr&STATUS_V) ? 0x3d11 : 0x3d07; return true;
        case 0x3d07: mcu.pc = 0x3d13; return true;
        case 0x3d09: mcu.r[4] = add(mcu.r[4],mcu.r[6]); mcu.pc += 2; return true;
        case 0x3d11: mcu.r[4] = mcu.r[5]; nz(mcu.r[4]); mcu.pc += 2; return true;
        case 0x3d13: MCU_Write16(mcu,block+30,mcu.r[4]); nz(mcu.r[4]); mcu.pc += 3; return true;
        case 0x3ccc: case 0x3d16: MCU_Write16(mcu,block+32,mcu.r[4]); nz(mcu.r[4]); mcu.pc += 3; return true;
        case 0x3ccf: case 0x3d19:
            if ((mcu.r[7]&1) || mcu.r[7] < 0x8000 || mcu.r[7] > 0xdffe) return false;
            ++mcu.pc; mcu.pc = MCU_PopStack(mcu); return true;
        default: return false;
    }
}

inline bool TryStepFirstModulationCopy(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0], source = mcu.r[2];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto sourceValid = [&] { return source >= 0xacde && source <= 0xc7a4 && (source-0xacde)%0x12a == 0; };
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    if (mcu.pc >= 0x3d32 && mcu.pc <= 0x3d65) {
        const unsigned relative = mcu.pc-0x3d32;
        if (relative%3) return false;
        const unsigned distance = 112-(relative/6)*2;
        if (relative%6 == 0) {
            if (!sourceValid()) return false;
            mcu.r[6] = ReadWord(mcu,source-distance);
        } else MCU_Write16(mcu,voice-distance,mcu.r[6]);
        nz(mcu.r[6]); mcu.pc += 3; return true;
    }
    switch (mcu.pc) {
        case 0x3d1a:
            if (!sourceValid()) return false;
            mcu.r[1] = ReadWord(mcu,source-2); nz(mcu.r[1]); mcu.pc += 3; return true;
        case 0x3d1d: case 0x3d26: {
            const unsigned before = mcu.r[1], result = before*2;
            mcu.r[1] = uint16_t(result); nz(mcu.r[1]);
            mcu.sr = uint16_t((mcu.sr&~3)|(result&65536 ? STATUS_C : 0)|((before^result)&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x3d1f: case 0x3d28:
            if (mcu.r[1] > 46 || (mcu.r[1]&1)) return false;
            if (mcu.pc == 0x3d1f) mcu.r[6] = ReadWord(mcu,0xc84e + mcu.r[1]);
            else MCU_Write16(mcu,0xc84e + mcu.r[1],mcu.r[6]);
            nz(mcu.r[6]); mcu.pc += 4; return true;
        case 0x3d23: mcu.r[1] = ReadWord(mcu,voice-2); nz(mcu.r[1]); mcu.pc += 3; return true;
        case 0x3d2c: case 0x3d68:
            if (!sourceValid()) return false;
            mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|MCU_Read(mcu,source-(mcu.pc == 0x3d2c ? 25 : 115)));
            nz(mcu.r[6],true); mcu.pc += 3; return true;
        case 0x3d2f: case 0x3d6b:
            MCU_Write(mcu,voice-(mcu.pc == 0x3d2f ? 25 : 115),uint8_t(mcu.r[6]));
            nz(mcu.r[6],true); mcu.pc += 3; return true;
        case 0x3d6e:
            if (!sourceValid()) return false;
            mcu.r[6] = ReadWord(mcu,source-114); nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x3d71: MCU_Write16(mcu,voice-114,mcu.r[6]); nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x3d74: case 0x3d86:
            mcu.r[6] = ReadWord(mcu,voice-(mcu.pc == 0x3d74 ? 104 : 102)); nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x3d77: case 0x3d89: {
            const unsigned value = mcu.r[6], result = uint16_t(value-65535);
            mcu.sr = uint16_t((mcu.sr&~15)|(result&32768 ? STATUS_N : 0)|(!result ? STATUS_Z : 0)
                |(value != 65535 ? STATUS_C : 0)|(value == 32767 ? STATUS_V : 0));
            mcu.pc += 3; return true;
        }
        case 0x3d7a: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3d86 : 0x3d7c; return true;
        case 0x3d8c: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3e1e : 0x3d8f; return true;
        case 0x3d7c: case 0x3d7f: case 0x3d82:
            MCU_Write16(mcu,voice-(122-((mcu.pc-0x3d7c)/3)*2),0);
            mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 3; return true;
        case 0x3d8f: case 0x3da1:
            mcu.r[2] = ReadWord(mcu,voice-(mcu.pc == 0x3d8f ? 128 : 126)); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3d92: mcu.pc = (mcu.sr&STATUS_N) ? 0x3d94 : 0x3d9c; return true;
        case 0x3da4: mcu.pc = (mcu.sr&STATUS_N) ? 0x3da6 : 0x3dae; return true;
        case 0x3d94: case 0x3d98: case 0x3da6: case 0x3daa: {
            const unsigned previous = mcu.r[2]; mcu.r[2] = uint16_t(0u-previous);
            mcu.sr = uint16_t((mcu.sr&~15)|(mcu.r[2]&32768 ? STATUS_N : 0)|(!mcu.r[2] ? STATUS_Z : 0)
                |(previous ? STATUS_C : 0)|(previous == 32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x3d96: case 0x3d9c: case 0x3da8: case 0x3dae: {
            const uint32_t product = uint32_t(mcu.r[2])*mcu.r[6];
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += 2; return true;
        }
        case 0x3d9a: mcu.pc = 0x3d9e; return true;
        case 0x3dac: mcu.pc = 0x3db0; return true;
        case 0x3d9e: case 0x3db0:
            MCU_Write16(mcu,voice-(mcu.pc == 0x3d9e ? 122 : 120),mcu.r[2]); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3d85:
            if ((mcu.r[7]&1) || mcu.r[7] < 0x8000 || mcu.r[7] > 0xdffe) return false;
            ++mcu.pc; mcu.pc = MCU_PopStack(mcu); return true;
        default: return false;
    }
}

inline bool TryStepSharedThirdDepth(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = true) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    auto byteMove = [&](unsigned reg, unsigned value) {
        mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|(value&255)); nz(value);
    };
    auto arithmetic = [&](unsigned reg, unsigned a, unsigned b, bool sub, bool byte = true) {
        const unsigned mask = byte ? 255 : 65535, sign = byte ? 128 : 32768;
        a &= mask; b &= mask;
        const unsigned wide = sub ? a-b : a+b, value = wide&mask;
        mcu.r[reg] = uint16_t((byte ? mcu.r[reg]&0xff00 : 0)|value);
        mcu.sr = uint16_t((mcu.sr&~15)|(value&sign ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
            |(wide&(mask+1) ? STATUS_C : 0)
            |((sub ? (a^b)&(a^value) : ~(a^b)&(a^value))&sign ? STATUS_V : 0));
    };
    switch (mcu.pc) {
        case 0x3dbb: mcu.r[2] = ReadWord(mcu,voice+168); nz(mcu.r[2],false); mcu.pc += 4; return true;
        case 0x3dbf: mcu.sr &= ~STATUS_C; nz(mcu.r[2]); mcu.pc += 2; return true;
        case 0x3dc1: mcu.pc = (mcu.sr&STATUS_N) ? 0x3dc3 : 0x3dcd; return true;
        case 0x3dc3: byteMove(2,mcu.r[2]&127); mcu.pc += 3; return true;
        case 0x3dc6: case 0x3dcd: arithmetic(3,mcu.r[3],64,true); mcu.pc += 3; return true;
        case 0x3dc9: mcu.pc = (mcu.sr&STATUS_C) ? 0x3dcb : 0x3df4; return true;
        case 0x3dcb: mcu.pc = 0x3de8; return true;
        case 0x3dd0: mcu.pc = (mcu.sr&STATUS_C) ? 0x3dd2 : 0x3dde; return true;
        case 0x3dd2: case 0x3de8: arithmetic(3,0,mcu.r[3],true); mcu.pc += 2; return true;
        case 0x3dd4: case 0x3dde: case 0x3dea: case 0x3df4: arithmetic(3,mcu.r[3],mcu.r[3],false); mcu.pc += 2; return true;
        case 0x3dd6: case 0x3df6: arithmetic(2,mcu.r[2],mcu.r[3],true); mcu.pc += 2; return true;
        case 0x3de0: case 0x3dec: arithmetic(2,mcu.r[2],mcu.r[3],false); mcu.pc += 2; return true;
        case 0x3dd8: case 0x3de2: mcu.pc = (mcu.sr&STATUS_N) ? uint16_t(mcu.pc+2) : 0x3e0f; return true;
        case 0x3dee: case 0x3df8: mcu.pc = (mcu.sr&STATUS_N) ? uint16_t(mcu.pc+2) : 0x3dfc; return true;
        case 0x3dda: case 0x3dfa: mcu.r[2] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x3de4: case 0x3df0: byteMove(2,127); mcu.pc += 2; return true;
        case 0x3ddc: case 0x3de6: mcu.pc = 0x3e0f; return true;
        case 0x3df2: mcu.pc = 0x3dfc; return true;
        case 0x3dfc: case 0x3e0f: arithmetic(2,mcu.r[2],mcu.r[2],false,false); mcu.pc += 2; return true;
        case 0x3dfe: case 0x3e11:
            if (mcu.r[2] > 254 || (mcu.r[2]&1)) return false;
            mcu.r[2] = ReadWord(mcu,0x7312+mcu.r[2]); nz(mcu.r[2],false); mcu.pc += 4; return true;
        case 0x3e02: arithmetic(2,0,mcu.r[2],true,false); mcu.pc += 2; return true;
        case 0x3e04: MCU_Write16(mcu,voice-124,mcu.r[2]); nz(mcu.r[2],false); mcu.pc += 3; return true;

        case 0x3db3: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x3db5: mcu.r[2] = ReadWord(mcu,voice+46); nz(mcu.r[2],false); mcu.pc += 3; return true;
        case 0x3db8: {
            const unsigned address = uint16_t(mcu.r[2]+17);
            if (address < 0x8000 || address > 0xdfff) return false;
            byteMove(3,MCU_Read(mcu,address)); mcu.pc += 3; return true;
        }
        case 0x3e07: case 0x3e0b: arithmetic(2,0,mcu.r[2],true,false); mcu.pc += 2; return true;
        case 0x3e09: case 0x3e18: {
            const uint32_t product = uint32_t(mcu.r[2])*mcu.r[6];
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += 2; return true;
        }
        case 0x3e0d: mcu.pc = 0x3e1a; return true;
        case 0x3e15: MCU_Write16(mcu,voice-124,mcu.r[2]); nz(mcu.r[2],false); mcu.pc += 3; return true;
        case 0x3e1a: MCU_Write16(mcu,voice-118,mcu.r[2]); nz(mcu.r[2],false); mcu.pc += 3; return true;
        case 0x3e1d:
            if ((mcu.r[7]&1) || mcu.r[7] < 0x8000 || mcu.r[7] > 0xdffe) return false;
            ++mcu.pc; mcu.pc = MCU_PopStack(mcu); return true;
        case 0x3e1e: case 0x3e24:
            mcu.r[6] = ReadWord(mcu,voice-(mcu.pc == 0x3e1e ? 128 : 126)); nz(mcu.r[6],false); mcu.pc += 3; return true;
        case 0x3e21: case 0x3e27:
            MCU_Write16(mcu,voice-(mcu.pc == 0x3e21 ? 122 : 120),mcu.r[6]); nz(mcu.r[6],false); mcu.pc += 3; return true;
        case 0x3e2a: mcu.r[6] = 65535; nz(mcu.r[6],false); mcu.pc += 3; return true;
        case 0x3e2d: mcu.pc = 0x3db3; return true;
        default: return false;
    }
}

inline bool TryStepFilterEntry(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    const bool romPage = mcu.pc == 0x3e8d || mcu.pc == 0x3e91 || mcu.pc == 0x3e93 || mcu.pc == 0x3e95;
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp != (romPage ? 3 : 0) || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x3e30: case 0x3e6f: mcu.r[1] = ReadWord(mcu,voice-2); nz(mcu.r[1]); mcu.pc += 3; return true;
        case 0x3e33: mcu.ep = MCU_Read(mcu,voice+153); mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x3e37: mcu.r[5] = ReadWord(mcu,voice+158); nz(mcu.r[5]); mcu.pc += 4; return true;
        case 0x3e3b: case 0x3e69: case 0x3e72: case 0x3e81: case 0x3e9a: case 0x3edf: {
            const unsigned offset = mcu.pc == 0x3e3b ? 39 : mcu.pc == 0x3e69 ? 38 : mcu.pc == 0x3e72 ? 37 : mcu.pc == 0x3e9a ? 41 : mcu.pc == 0x3edf ? 61 : 40;
            const unsigned reg = mcu.pc == 0x3e72 ? 2 : (mcu.pc == 0x3e81 || mcu.pc == 0x3e9a || mcu.pc == 0x3edf) ? 3 : 4;
            const unsigned address = uint16_t(mcu.r[5]+offset);
            if (mcu.ep > 4 || (!mcu.ep && address >= 0xe000)) return false;
            const unsigned value = MCU_Read(mcu,(unsigned(mcu.ep)<<16)|address);
            mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|value); nz(value,true); mcu.pc += 3; return true;
        }
        case 0x3e3e: {
            const unsigned a = mcu.r[4]&255, value = (a-2)&255;
            mcu.sr = uint16_t((mcu.sr&~15)|(value&128 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(a < 2 ? STATUS_C : 0)|((a^2)&(a^value)&128 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x3e40: mcu.pc = (mcu.sr&STATUS_C) ? 0x3e5e : 0x3e42; return true;
        // Five-byte immediate MOV; the linear disassembly loses alignment here.
        case 0x3e42: MCU_Write(mcu,voice+101,255); nz(255,true); mcu.pc += 5; return true;
        case 0x3e47: case 0x3e4a: case 0x3e4d: case 0x3e50: {
            const int offset = mcu.pc == 0x3e47 ? -24 : mcu.pc == 0x3e4a ? -22 : mcu.pc == 0x3e4d ? 38 : 36;
            MCU_Write16(mcu,voice+offset,0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 3; return true;
        }
        case 0x3e53: case 0x3e57: {
            const bool first = mcu.pc == 0x3e53;
            MCU_Write(mcu,voice+(first ? 104 : 102),first ? 64 : 3); nz(first ? 64 : 3,true); mcu.pc += 4; return true;
        }
        case 0x3e5b: mcu.pc = 0x4435; return true;
        case 0x3e5e: MCU_Write(mcu,voice+101,0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 3; return true;
        case 0x3e61: case 0x3e7f: case 0x3e88: case 0x3e91: case 0x3eb7: case 0x3ec7: case 0x3eee: case 0x3efa: {
            const bool byte = mcu.pc == 0x3e61;
            const unsigned reg = byte ? 4 : mcu.pc == 0x3e7f ? 2 : 3;
            const unsigned mask = byte ? 255 : 65535, sign = byte ? 128 : 32768;
            const unsigned a = mcu.r[reg]&mask, b = mcu.r[mcu.pc == 0x3e91 ? 2 : reg]&mask;
            const unsigned sum = a+b, value = sum&mask;
            mcu.r[reg] = uint16_t((byte ? mcu.r[reg]&0xff00 : 0)|value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&sign ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(sum > mask ? STATUS_C : 0)|(~(a^b)&(a^value)&sign ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x3e63: mcu.r[4] |= 1; nz(mcu.r[4],true); mcu.pc += 3; return true;
        case 0x3e66: case 0x3e6c: case 0x3e75: {
            const unsigned offset = mcu.pc == 0x3e66 ? 102 : mcu.pc == 0x3e6c ? 103 : 163;
            const unsigned value = mcu.r[mcu.pc == 0x3e75 ? 2 : 4]&255;
            MCU_Write(mcu,voice+offset,value); nz(value,true); mcu.pc += offset == 163 ? 4 : 3; return true;
        }
        case 0x3e79: mcu.r[2] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x3e7b:
            if (mcu.r[1] >= 24) return false;
            mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|MCU_Read(mcu,0xc8fc+mcu.r[1])); nz(mcu.r[2],true); mcu.pc += 4; return true;
        case 0x3e84: mcu.r[3] &= 15; nz(mcu.r[3]); mcu.pc += 4; return true;
        case 0x3e8a: case 0x3e95:
            mcu.dp = mcu.pc == 0x3e8a ? 3 : 0; mcu.ex_ignore = 1; mcu.pc += 3; return true;
        case 0x3e8d:
            if (mcu.r[3] > 30 || (mcu.r[3]&1)) return false;
            mcu.r[3] = MCU_Read16(mcu,0x30000|uint16_t(0xdcd2+mcu.r[3])); nz(mcu.r[3]); mcu.pc += 4; return true;
        case 0x3e93:
            if (mcu.r[3]&1) return false;
            mcu.r[2] = MCU_Read16(mcu,0x30000|mcu.r[3]); nz(mcu.r[2]); mcu.pc += 2; return true;
        case 0x3e98: case 0x3edd: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x3e9d: case 0x3ea5: case 0x3eaf: case 0x3ea3: case 0x3eab: case 0x3eb5: case 0x3ec3: {
            const bool immediate = mcu.pc == 0x3e9d || mcu.pc == 0x3ea5 || mcu.pc == 0x3eaf;
            const unsigned reg = mcu.pc == 0x3e9d || mcu.pc == 0x3ea3 || mcu.pc == 0x3ec3 ? 3 : 2;
            const unsigned a = immediate ? mcu.r[reg] : 0;
            const unsigned b = immediate ? (reg == 3 ? 64 : 16384) : mcu.r[reg];
            const unsigned value = (a-b)&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(a < b ? STATUS_C : 0)|((a^b)&(a^value)&32768 ? STATUS_V : 0));
            mcu.pc += immediate ? 4 : 2; return true;
        }
        case 0x3ea1: case 0x3ea9: case 0x3eb3: {
            const unsigned target = mcu.pc == 0x3ea1 ? 0x3eaf : mcu.pc == 0x3ea9 ? 0x3eb7 : 0x3ec7;
            mcu.pc = uint16_t((mcu.sr&STATUS_C) ? mcu.pc+2 : target); return true;
        }
        case 0x3ead: mcu.pc = 0x3ec7; return true;
        case 0x3ec5: mcu.pc = 0x3ed3; return true;
        case 0x3eb9: case 0x3ec9:
            if ((mcu.r[3]&1) || mcu.r[3] > 128) return false;
            mcu.r[3] = ReadWord(mcu,0x74d2+mcu.r[3]); nz(mcu.r[3]); mcu.pc += 4; return true;
        case 0x3ebd: case 0x3ecd: {
            const uint32_t product = uint32_t(mcu.r[2])*mcu.r[3];
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += 2; return true;
        }
        case 0x3ebf: case 0x3ecf:
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|(mcu.r[2]&255)); nz(mcu.r[3],true); mcu.pc += 2; return true;
        case 0x3ec1: case 0x3ed1:
            mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x3ed3: MCU_Write16(mcu,voice+84,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x3ed6: mcu.r[2] = 127; nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3ee2: mcu.r[4] = 32767; nz(mcu.r[4]); mcu.pc += 3; return true;
        case 0x3ed9: case 0x3ee5: case 0x3eec: case 0x3ef4: case 0x3ef6: case 0x3f00: {
            const bool byte = mcu.pc == 0x3ed9 || mcu.pc == 0x3ee5 || mcu.pc == 0x3eec;
            const bool negate = mcu.pc == 0x3eec || mcu.pc == 0x3ef6;
            const unsigned reg = mcu.pc == 0x3ed9 ? 2 : byte ? 3 : 4;
            const unsigned mask = byte ? 255 : 65535, sign = byte ? 128 : 32768;
            if (mcu.pc == 0x3ed9 && mcu.r[1] >= 24) return false;
            const unsigned a = negate ? 0 : mcu.r[reg]&mask;
            const unsigned b = (negate ? mcu.r[reg] : mcu.pc == 0x3ed9 ? MCU_Read(mcu,0xc95c+mcu.r[1]) : mcu.pc == 0x3ee5 ? 64 : mcu.r[3])&mask;
            const unsigned value = (a-b)&mask;
            mcu.r[reg] = uint16_t((byte ? mcu.r[reg]&0xff00 : 0)|value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&sign ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(a < b ? STATUS_C : 0)|((a^b)&(a^value)&sign ? STATUS_V : 0));
            mcu.pc += mcu.pc == 0x3ed9 ? 4 : mcu.pc == 0x3ee5 ? 3 : 2; return true;
        }
        case 0x3ee8: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3f02 : 0x3eea; return true;
        case 0x3eea: mcu.pc = (mcu.sr&STATUS_C) ? 0x3eec : 0x3efa; return true;
        case 0x3ef8: mcu.pc = 0x3f02; return true;
        case 0x3ef0: case 0x3efc: {
            if ((mcu.r[3]&1) || mcu.r[3] > 128) return false;
            const uint32_t product = uint32_t(mcu.r[2])*ReadWord(mcu,0x74fc+mcu.r[3]);
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += 4; return true;
        }
        case 0x3f02: MCU_Write16(mcu,voice-46,mcu.r[4]); nz(mcu.r[4]); mcu.pc += 3; return true;
        default: return false;
    }
}

inline bool TryStepFilterDepth(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x3f05: mcu.r[2] = ReadWord(mcu,voice-46); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3f08: case 0x3f16: case 0x3f37: case 0x3f69:
            mcu.r[mcu.pc == 0x3f16 ? 6 : 3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x3f0a: case 0x3f18: {
            const bool first = mcu.pc == 0x3f0a;
            const unsigned reg = first ? 3 : 6, address = uint16_t(mcu.r[5]+(first ? 44 : 45));
            if (mcu.ep > 4 || (!mcu.ep && address >= 0xe000)) return false;
            const unsigned value = MCU_Read(mcu,(unsigned(mcu.ep)<<16)|address);
            mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|value); nz(value,true); mcu.pc += 3; return true;
        }
        case 0x3f0d: case 0x3f33: case 0x3f41: case 0x3f65: case 0x3f73: {
            const unsigned a = mcu.r[3], sum = a+a, value = sum&65535;
            mcu.r[3] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|((a^value)&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x3f0f:
            if ((mcu.r[3]&1) || mcu.r[3] > 510) return false;
            mcu.r[3] = ReadWord(mcu,0x7512+mcu.r[3]); nz(mcu.r[3]); mcu.pc += 4; return true;
        case 0x3f13: MCU_Write16(mcu,voice-36,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x3f1b: case 0x3f21: case 0x3f27: case 0x3f2f: {
            const bool immediate = mcu.pc == 0x3f1b;
            const unsigned reg = (immediate || mcu.pc == 0x3f21) ? 6 : 2;
            const unsigned a = immediate ? mcu.r[reg] : 0, b = immediate ? 64 : mcu.r[reg];
            const unsigned value = (a-b)&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(a < b ? STATUS_C : 0)|((a^b)&(a^value)&32768 ? STATUS_V : 0));
            mcu.pc += immediate ? 4 : 2; return true;
        }
        case 0x3f1f: mcu.pc = (mcu.sr&STATUS_C) ? 0x3f21 : 0x3f2b; return true;
        case 0x3f23: case 0x3f2b:
            mcu.sr = uint16_t((mcu.sr&~STATUS_Z)|((mcu.r[2]&32768) ? 0 : STATUS_Z)); mcu.pc += 2; return true;
        case 0x3f25: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3f31 : 0x3f27; return true;
        case 0x3f2d: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3f63 : 0x3f2f; return true;
        case 0x3f29: mcu.pc = 0x3f63; return true;
        case 0x3f31: case 0x3f3f: case 0x3f63: case 0x3f71: {
            const uint32_t product = uint32_t(mcu.r[2])*mcu.r[3];
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += 2; return true;
        }
        case 0x3f35: case 0x3f43: case 0x3f67: case 0x3f75: {
            const unsigned a = mcu.r[2], carry = (mcu.sr&STATUS_C) ? 1 : 0;
            const bool oldZero = (mcu.sr&STATUS_Z) != 0;
            const unsigned sum = a+a+carry, value = sum&65535;
            const int signedSum = int(int16_t(a))*2+int(carry);
            mcu.r[2] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value && oldZero ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|(signedSum < -32768 || signedSum > 32767 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x3f39: case 0x3f6b: case 0x3f45: case 0x3f77: {
            const unsigned reg = mcu.pc == 0x3f39 || mcu.pc == 0x3f6b ? 6 : 2;
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|(mcu.r[reg]&255)); nz(mcu.r[3],true); mcu.pc += 2; return true;
        }
        case 0x3f3b: case 0x3f6d:
            if (mcu.r[3] > 255) return false;
            mcu.r[3] = MCU_Read(mcu,0x79f2+mcu.r[3]); nz(mcu.r[3],true); mcu.pc += 4; return true;
        case 0x3f47: case 0x3f79:
            mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x3f49: case 0x3f7b:
            mcu.r[6] = ReadWord(mcu,voice+84); nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x3f4c: mcu.pc = (mcu.sr&STATUS_N) ? 0x3f54 : 0x3f4e; return true;
        case 0x3f58: mcu.pc = (mcu.sr&STATUS_N) ? 0x3f5e : 0x3f5a; return true;
        case 0x3f7e: mcu.pc = (mcu.sr&STATUS_N) ? 0x3f89 : 0x3f80; return true;
        case 0x3f82: mcu.pc = (mcu.sr&STATUS_N) ? 0x3f84 : 0x3f8d; return true;
        case 0x3f4e: {
            const auto value = mcu.r[3]; mcu.r[3] = mcu.r[6]; mcu.r[6] = value; mcu.pc += 2; return true;
        }
        case 0x3f50: case 0x3f54: case 0x3f56: case 0x3f5a: case 0x3f80: case 0x3f89: case 0x3f8b: {
            const bool add = mcu.pc == 0x3f56 || mcu.pc == 0x3f80;
            const bool negate = mcu.pc == 0x3f54 || mcu.pc == 0x3f5a || mcu.pc == 0x3f89;
            const unsigned reg = mcu.pc == 0x3f54 || mcu.pc == 0x3f89 ? 6 : 3;
            const unsigned a = negate ? 0 : mcu.r[reg], b = mcu.r[negate ? reg : 6];
            const unsigned wide = add ? a+b : a-b, value = wide&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(wide&65536 ? STATUS_C : 0)
                |((add ? ~(a^b)&(a^value) : (a^b)&(a^value))&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x3f52: case 0x3f5c: case 0x3f61: case 0x3f87: mcu.pc = 0x3f8d; return true;
        case 0x3f5e: case 0x3f84:
            mcu.r[3] = mcu.pc == 0x3f5e ? 0x8001 : 0x7fff; nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x3f8d: MCU_Write16(mcu,voice+86,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        default: return false;
    }
}

inline bool TryStepSecondFilterDepth(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x3f90: mcu.r[2] = ReadWord(mcu,voice-46); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x3f93: case 0x3fb5: case 0x3fe8:
            mcu.r[mcu.pc == 0x3f93 ? 6 : 3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x3f95: {
            const unsigned reg = 6, address = uint16_t(mcu.r[5]+46);
            if (mcu.ep > 4 || (!mcu.ep && address >= 0xe000)) return false;
            const unsigned value = MCU_Read(mcu,(unsigned(mcu.ep)<<16)|address);
            mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|value); nz(value,true); mcu.pc += 3; return true;
        }
        case 0x3fb1: case 0x3fbf: case 0x3fe4: case 0x3ff2: {
            const unsigned a = mcu.r[3], sum = a+a, value = sum&65535;
            mcu.r[3] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|((a^value)&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x3f98: case 0x3f9e: case 0x3fa4: case 0x3fac: {
            const bool immediate = mcu.pc == 0x3f98;
            const unsigned reg = (immediate || mcu.pc == 0x3f9e) ? 6 : 2;
            const unsigned a = immediate ? mcu.r[reg] : 0, b = immediate ? 64 : mcu.r[reg];
            const unsigned value = (a-b)&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(a < b ? STATUS_C : 0)|((a^b)&(a^value)&32768 ? STATUS_V : 0));
            mcu.pc += immediate ? 4 : 2; return true;
        }
        case 0x3f9c: mcu.pc = (mcu.sr&STATUS_C) ? 0x3f9e : 0x3fa8; return true;
        case 0x3fa0: case 0x3fa8:
            mcu.sr = uint16_t((mcu.sr&~STATUS_Z)|((mcu.r[2]&32768) ? 0 : STATUS_Z)); mcu.pc += 2; return true;
        case 0x3fa2: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3fae : 0x3fa4; return true;
        case 0x3faa: mcu.pc = (mcu.sr&STATUS_Z) ? 0x3fe1 : 0x3fac; return true;
        case 0x3fa6: mcu.pc = 0x3fe1; return true;
        case 0x3fae: case 0x3fbd: case 0x3fe1: case 0x3ff0: {
            const bool memory = mcu.pc == 0x3fae || mcu.pc == 0x3fe1;
            const uint32_t product = uint32_t(mcu.r[2])*(memory ? ReadWord(mcu,voice-36) : mcu.r[3]);
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += memory ? 3 : 2; return true;
        }
        case 0x3fb3: case 0x3fc1: case 0x3fe6: case 0x3ff4: {
            const unsigned a = mcu.r[2], carry = (mcu.sr&STATUS_C) ? 1 : 0;
            const bool oldZero = (mcu.sr&STATUS_Z) != 0;
            const unsigned sum = a+a+carry, value = sum&65535;
            const int signedSum = int(int16_t(a))*2+int(carry);
            mcu.r[2] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value && oldZero ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|(signedSum < -32768 || signedSum > 32767 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x3fb7: case 0x3fea: case 0x3fc3: case 0x3ff6: {
            const unsigned reg = mcu.pc == 0x3fb7 || mcu.pc == 0x3fea ? 6 : 2;
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|(mcu.r[reg]&255)); nz(mcu.r[3],true); mcu.pc += 2; return true;
        }
        case 0x3fb9: case 0x3fec:
            if (mcu.r[3] > 255) return false;
            mcu.r[3] = MCU_Read(mcu,0x79f2+mcu.r[3]); nz(mcu.r[3],true); mcu.pc += 4; return true;
        case 0x3fc5: case 0x3ff8:
            mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x3fc7: case 0x3ffa:
            mcu.r[6] = ReadWord(mcu,voice+84); nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x3fca: mcu.pc = (mcu.sr&STATUS_N) ? 0x3fd2 : 0x3fcc; return true;
        case 0x3fd6: mcu.pc = (mcu.sr&STATUS_N) ? 0x3fdc : 0x3fd8; return true;
        case 0x3ffd: mcu.pc = (mcu.sr&STATUS_N) ? 0x4008 : 0x3fff; return true;
        case 0x4001: mcu.pc = (mcu.sr&STATUS_N) ? 0x4003 : 0x400c; return true;
        case 0x3fcc: {
            const auto value = mcu.r[3]; mcu.r[3] = mcu.r[6]; mcu.r[6] = value; mcu.pc += 2; return true;
        }
        case 0x3fce: case 0x3fd2: case 0x3fd4: case 0x3fd8: case 0x3fff: case 0x4008: case 0x400a: {
            const bool add = mcu.pc == 0x3fd4 || mcu.pc == 0x3fff;
            const bool negate = mcu.pc == 0x3fd2 || mcu.pc == 0x3fd8 || mcu.pc == 0x4008;
            const unsigned reg = mcu.pc == 0x3fd2 || mcu.pc == 0x4008 ? 6 : 3;
            const unsigned a = negate ? 0 : mcu.r[reg], b = mcu.r[negate ? reg : 6];
            const unsigned wide = add ? a+b : a-b, value = wide&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(wide&65536 ? STATUS_C : 0)
                |((add ? ~(a^b)&(a^value) : (a^b)&(a^value))&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x3fd0: case 0x3fda: case 0x3fdf: case 0x4006: mcu.pc = 0x400c; return true;
        case 0x3fdc: case 0x4003:
            mcu.r[3] = mcu.pc == 0x3fdc ? 0x8001 : 0x7fff; nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x400c: MCU_Write16(mcu,voice+88,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        default: return false;
    }
}

inline bool TryStepThirdFilterDepth(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x400f: mcu.r[2] = ReadWord(mcu,voice-46); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x4012: case 0x4034: case 0x4067:
            mcu.r[mcu.pc == 0x4012 ? 6 : 3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x4014: {
            const unsigned reg = 6, address = uint16_t(mcu.r[5]+47);
            if (mcu.ep > 4 || (!mcu.ep && address >= 0xe000)) return false;
            const unsigned value = MCU_Read(mcu,(unsigned(mcu.ep)<<16)|address);
            mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|value); nz(value,true); mcu.pc += 3; return true;
        }
        case 0x4030: case 0x403e: case 0x4063: case 0x4071: {
            const unsigned a = mcu.r[3], sum = a+a, value = sum&65535;
            mcu.r[3] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|((a^value)&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x4017: case 0x401d: case 0x4023: case 0x402b: {
            const bool immediate = mcu.pc == 0x4017;
            const unsigned reg = (immediate || mcu.pc == 0x401d) ? 6 : 2;
            const unsigned a = immediate ? mcu.r[reg] : 0, b = immediate ? 64 : mcu.r[reg];
            const unsigned value = (a-b)&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(a < b ? STATUS_C : 0)|((a^b)&(a^value)&32768 ? STATUS_V : 0));
            mcu.pc += immediate ? 4 : 2; return true;
        }
        case 0x401b: mcu.pc = (mcu.sr&STATUS_C) ? 0x401d : 0x4027; return true;
        case 0x401f: case 0x4027:
            mcu.sr = uint16_t((mcu.sr&~STATUS_Z)|((mcu.r[2]&32768) ? 0 : STATUS_Z)); mcu.pc += 2; return true;
        case 0x4021: mcu.pc = (mcu.sr&STATUS_Z) ? 0x402d : 0x4023; return true;
        case 0x4029: mcu.pc = (mcu.sr&STATUS_Z) ? 0x4060 : 0x402b; return true;
        case 0x4025: mcu.pc = 0x4060; return true;
        case 0x402d: case 0x403c: case 0x4060: case 0x406f: {
            const bool memory = mcu.pc == 0x402d || mcu.pc == 0x4060;
            const uint32_t product = uint32_t(mcu.r[2])*(memory ? ReadWord(mcu,voice-36) : mcu.r[3]);
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += memory ? 3 : 2; return true;
        }
        case 0x4032: case 0x4040: case 0x4065: case 0x4073: {
            const unsigned a = mcu.r[2], carry = (mcu.sr&STATUS_C) ? 1 : 0;
            const bool oldZero = (mcu.sr&STATUS_Z) != 0;
            const unsigned sum = a+a+carry, value = sum&65535;
            const int signedSum = int(int16_t(a))*2+int(carry);
            mcu.r[2] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value && oldZero ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|(signedSum < -32768 || signedSum > 32767 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x4036: case 0x4069: case 0x4042: case 0x4075: {
            const unsigned reg = mcu.pc == 0x4036 || mcu.pc == 0x4069 ? 6 : 2;
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|(mcu.r[reg]&255)); nz(mcu.r[3],true); mcu.pc += 2; return true;
        }
        case 0x4038: case 0x406b:
            if (mcu.r[3] > 255) return false;
            mcu.r[3] = MCU_Read(mcu,0x79f2+mcu.r[3]); nz(mcu.r[3],true); mcu.pc += 4; return true;
        case 0x4044: case 0x4077:
            mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x4046: case 0x4079:
            mcu.r[6] = ReadWord(mcu,voice+84); nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x4049: mcu.pc = (mcu.sr&STATUS_N) ? 0x4051 : 0x404b; return true;
        case 0x4055: mcu.pc = (mcu.sr&STATUS_N) ? 0x405b : 0x4057; return true;
        case 0x407c: mcu.pc = (mcu.sr&STATUS_N) ? 0x4087 : 0x407e; return true;
        case 0x4080: mcu.pc = (mcu.sr&STATUS_N) ? 0x4082 : 0x408b; return true;
        case 0x404b: {
            const auto value = mcu.r[3]; mcu.r[3] = mcu.r[6]; mcu.r[6] = value; mcu.pc += 2; return true;
        }
        case 0x404d: case 0x4051: case 0x4053: case 0x4057: case 0x407e: case 0x4087: case 0x4089: {
            const bool add = mcu.pc == 0x4053 || mcu.pc == 0x407e;
            const bool negate = mcu.pc == 0x4051 || mcu.pc == 0x4057 || mcu.pc == 0x4087;
            const unsigned reg = mcu.pc == 0x4051 || mcu.pc == 0x4087 ? 6 : 3;
            const unsigned a = negate ? 0 : mcu.r[reg], b = mcu.r[negate ? reg : 6];
            const unsigned wide = add ? a+b : a-b, value = wide&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(wide&65536 ? STATUS_C : 0)
                |((add ? ~(a^b)&(a^value) : (a^b)&(a^value))&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x404f: case 0x4059: case 0x405e: case 0x4085: mcu.pc = 0x408b; return true;
        case 0x405b: case 0x4082:
            mcu.r[3] = mcu.pc == 0x405b ? 0x8001 : 0x7fff; nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x408b: MCU_Write16(mcu,voice+90,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        default: return false;
    }
}

inline bool TryStepFourthFilterDepth(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x408e: mcu.r[2] = ReadWord(mcu,voice-46); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x4091: case 0x40b3: case 0x40e6:
            mcu.r[mcu.pc == 0x4091 ? 6 : 3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x4093: {
            const unsigned reg = 6, address = uint16_t(mcu.r[5]+48);
            if (mcu.ep > 4 || (!mcu.ep && address >= 0xe000)) return false;
            const unsigned value = MCU_Read(mcu,(unsigned(mcu.ep)<<16)|address);
            mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|value); nz(value,true); mcu.pc += 3; return true;
        }
        case 0x40af: case 0x40bd: case 0x40e2: case 0x40f0: {
            const unsigned a = mcu.r[3], sum = a+a, value = sum&65535;
            mcu.r[3] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|((a^value)&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x4096: case 0x409c: case 0x40a2: case 0x40aa: {
            const bool immediate = mcu.pc == 0x4096;
            const unsigned reg = (immediate || mcu.pc == 0x409c) ? 6 : 2;
            const unsigned a = immediate ? mcu.r[reg] : 0, b = immediate ? 64 : mcu.r[reg];
            const unsigned value = (a-b)&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(a < b ? STATUS_C : 0)|((a^b)&(a^value)&32768 ? STATUS_V : 0));
            mcu.pc += immediate ? 4 : 2; return true;
        }
        case 0x409a: mcu.pc = (mcu.sr&STATUS_C) ? 0x409c : 0x40a6; return true;
        case 0x409e: case 0x40a6:
            mcu.sr = uint16_t((mcu.sr&~STATUS_Z)|((mcu.r[2]&32768) ? 0 : STATUS_Z)); mcu.pc += 2; return true;
        case 0x40a0: mcu.pc = (mcu.sr&STATUS_Z) ? 0x40ac : 0x40a2; return true;
        case 0x40a8: mcu.pc = (mcu.sr&STATUS_Z) ? 0x40df : 0x40aa; return true;
        case 0x40a4: mcu.pc = 0x40df; return true;
        case 0x40ac: case 0x40bb: case 0x40df: case 0x40ee: {
            const bool memory = mcu.pc == 0x40ac || mcu.pc == 0x40df;
            const uint32_t product = uint32_t(mcu.r[2])*(memory ? ReadWord(mcu,voice-36) : mcu.r[3]);
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += memory ? 3 : 2; return true;
        }
        case 0x40b1: case 0x40bf: case 0x40e4: case 0x40f2: {
            const unsigned a = mcu.r[2], carry = (mcu.sr&STATUS_C) ? 1 : 0;
            const bool oldZero = (mcu.sr&STATUS_Z) != 0;
            const unsigned sum = a+a+carry, value = sum&65535;
            const int signedSum = int(int16_t(a))*2+int(carry);
            mcu.r[2] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value && oldZero ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|(signedSum < -32768 || signedSum > 32767 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x40b5: case 0x40e8: case 0x40c1: case 0x40f4: {
            const unsigned reg = mcu.pc == 0x40b5 || mcu.pc == 0x40e8 ? 6 : 2;
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|(mcu.r[reg]&255)); nz(mcu.r[3],true); mcu.pc += 2; return true;
        }
        case 0x40b7: case 0x40ea:
            if (mcu.r[3] > 255) return false;
            mcu.r[3] = MCU_Read(mcu,0x79f2+mcu.r[3]); nz(mcu.r[3],true); mcu.pc += 4; return true;
        case 0x40c3: case 0x40f6:
            mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x40c5: case 0x40f8:
            mcu.r[6] = ReadWord(mcu,voice+84); nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x40c8: mcu.pc = (mcu.sr&STATUS_N) ? 0x40d0 : 0x40ca; return true;
        case 0x40d4: mcu.pc = (mcu.sr&STATUS_N) ? 0x40da : 0x40d6; return true;
        case 0x40fb: mcu.pc = (mcu.sr&STATUS_N) ? 0x4106 : 0x40fd; return true;
        case 0x40ff: mcu.pc = (mcu.sr&STATUS_N) ? 0x4101 : 0x410a; return true;
        case 0x40ca: {
            const auto value = mcu.r[3]; mcu.r[3] = mcu.r[6]; mcu.r[6] = value; mcu.pc += 2; return true;
        }
        case 0x40cc: case 0x40d0: case 0x40d2: case 0x40d6: case 0x40fd: case 0x4106: case 0x4108: {
            const bool add = mcu.pc == 0x40d2 || mcu.pc == 0x40fd;
            const bool negate = mcu.pc == 0x40d0 || mcu.pc == 0x40d6 || mcu.pc == 0x4106;
            const unsigned reg = mcu.pc == 0x40d0 || mcu.pc == 0x4106 ? 6 : 3;
            const unsigned a = negate ? 0 : mcu.r[reg], b = mcu.r[negate ? reg : 6];
            const unsigned wide = add ? a+b : a-b, value = wide&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(wide&65536 ? STATUS_C : 0)
                |((add ? ~(a^b)&(a^value) : (a^b)&(a^value))&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x40ce: case 0x40d8: case 0x40dd: case 0x4104: mcu.pc = 0x410a; return true;
        case 0x40da: case 0x4101:
            mcu.r[3] = mcu.pc == 0x40da ? 0x8001 : 0x7fff; nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x410a: MCU_Write16(mcu,voice+92,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        default: return false;
    }
}

inline bool TryStepFifthFilterDepth(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x410d: mcu.r[2] = ReadWord(mcu,voice-46); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x4110: case 0x4132: case 0x4165:
            mcu.r[mcu.pc == 0x4110 ? 6 : 3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x4112: {
            const unsigned reg = 6, address = uint16_t(mcu.r[5]+49);
            if (mcu.ep > 4 || (!mcu.ep && address >= 0xe000)) return false;
            const unsigned value = MCU_Read(mcu,(unsigned(mcu.ep)<<16)|address);
            mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|value); nz(value,true); mcu.pc += 3; return true;
        }
        case 0x412e: case 0x413c: case 0x4161: case 0x416f: {
            const unsigned a = mcu.r[3], sum = a+a, value = sum&65535;
            mcu.r[3] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|((a^value)&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x4115: case 0x411b: case 0x4121: case 0x4129: {
            const bool immediate = mcu.pc == 0x4115;
            const unsigned reg = (immediate || mcu.pc == 0x411b) ? 6 : 2;
            const unsigned a = immediate ? mcu.r[reg] : 0, b = immediate ? 64 : mcu.r[reg];
            const unsigned value = (a-b)&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(a < b ? STATUS_C : 0)|((a^b)&(a^value)&32768 ? STATUS_V : 0));
            mcu.pc += immediate ? 4 : 2; return true;
        }
        case 0x4119: mcu.pc = (mcu.sr&STATUS_C) ? 0x411b : 0x4125; return true;
        case 0x411d: case 0x4125:
            mcu.sr = uint16_t((mcu.sr&~STATUS_Z)|((mcu.r[2]&32768) ? 0 : STATUS_Z)); mcu.pc += 2; return true;
        case 0x411f: mcu.pc = (mcu.sr&STATUS_Z) ? 0x412b : 0x4121; return true;
        case 0x4127: mcu.pc = (mcu.sr&STATUS_Z) ? 0x415e : 0x4129; return true;
        case 0x4123: mcu.pc = 0x415e; return true;
        case 0x412b: case 0x413a: case 0x415e: case 0x416d: {
            const bool memory = mcu.pc == 0x412b || mcu.pc == 0x415e;
            const uint32_t product = uint32_t(mcu.r[2])*(memory ? ReadWord(mcu,voice-36) : mcu.r[3]);
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += memory ? 3 : 2; return true;
        }
        case 0x4130: case 0x413e: case 0x4163: case 0x4171: {
            const unsigned a = mcu.r[2], carry = (mcu.sr&STATUS_C) ? 1 : 0;
            const bool oldZero = (mcu.sr&STATUS_Z) != 0;
            const unsigned sum = a+a+carry, value = sum&65535;
            const int signedSum = int(int16_t(a))*2+int(carry);
            mcu.r[2] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value && oldZero ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|(signedSum < -32768 || signedSum > 32767 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x4134: case 0x4167: case 0x4140: case 0x4173: {
            const unsigned reg = mcu.pc == 0x4134 || mcu.pc == 0x4167 ? 6 : 2;
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|(mcu.r[reg]&255)); nz(mcu.r[3],true); mcu.pc += 2; return true;
        }
        case 0x4136: case 0x4169:
            if (mcu.r[3] > 255) return false;
            mcu.r[3] = MCU_Read(mcu,0x79f2+mcu.r[3]); nz(mcu.r[3],true); mcu.pc += 4; return true;
        case 0x4142: case 0x4175:
            mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x4144: case 0x4177:
            mcu.r[6] = ReadWord(mcu,voice+84); nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x4147: mcu.pc = (mcu.sr&STATUS_N) ? 0x414f : 0x4149; return true;
        case 0x4153: mcu.pc = (mcu.sr&STATUS_N) ? 0x4159 : 0x4155; return true;
        case 0x417a: mcu.pc = (mcu.sr&STATUS_N) ? 0x4185 : 0x417c; return true;
        case 0x417e: mcu.pc = (mcu.sr&STATUS_N) ? 0x4180 : 0x4189; return true;
        case 0x4149: {
            const auto value = mcu.r[3]; mcu.r[3] = mcu.r[6]; mcu.r[6] = value; mcu.pc += 2; return true;
        }
        case 0x414b: case 0x414f: case 0x4151: case 0x4155: case 0x417c: case 0x4185: case 0x4187: {
            const bool add = mcu.pc == 0x4151 || mcu.pc == 0x417c;
            const bool negate = mcu.pc == 0x414f || mcu.pc == 0x4155 || mcu.pc == 0x4185;
            const unsigned reg = mcu.pc == 0x414f || mcu.pc == 0x4185 ? 6 : 3;
            const unsigned a = negate ? 0 : mcu.r[reg], b = mcu.r[negate ? reg : 6];
            const unsigned wide = add ? a+b : a-b, value = wide&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(wide&65536 ? STATUS_C : 0)
                |((add ? ~(a^b)&(a^value) : (a^b)&(a^value))&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x414d: case 0x4157: case 0x415c: case 0x4183: mcu.pc = 0x4189; return true;
        case 0x4159: case 0x4180:
            mcu.r[3] = mcu.pc == 0x4159 ? 0x8001 : 0x7fff; nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x4189: MCU_Write16(mcu,voice+94,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        default: return false;
    }
}

inline bool TryStepFilterMaximum(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    if (mcu.pc >= 0x418f && mcu.pc <= 0x41b4) {
        const unsigned index = (mcu.pc-0x418f)/8, step = (mcu.pc-0x418f)%8;
        if (index > 4) return false;
        const unsigned address = voice+86+index*2;
        if (step == 0) {
            const unsigned a = mcu.r[3], b = ReadWord(mcu,address), value = (a-b)&65535;
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(a < b ? STATUS_C : 0)|((a^b)&(a^value)&32768 ? STATUS_V : 0));
            mcu.pc += 3; return true;
        }
        if (step == 3) {
            const bool greater = !(mcu.sr&STATUS_Z) && bool(mcu.sr&STATUS_N) == bool(mcu.sr&STATUS_V);
            mcu.pc += greater ? 5 : 2; return true;
        }
        if (step == 5) {
            mcu.r[3] = ReadWord(mcu,address); nz(mcu.r[3]); mcu.pc += 3; return true;
        }
        return false;
    }
    switch (mcu.pc) {
        case 0x418c: mcu.r[3] = ReadWord(mcu,voice+84); nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x41b7: mcu.r[4] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x41b9:
            mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|MCU_Read(mcu,voice+163)); nz(mcu.r[4],true); mcu.pc += 4; return true;
        case 0x41bd: case 0x4227: mcu.r[2] = ReadWord(mcu,voice+46); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x41c0: {
            const unsigned address = uint16_t(mcu.r[2]+18);
            if (address >= 0xe000) return false;
            mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|MCU_Read(mcu,address)); nz(mcu.r[6],true); mcu.pc += 3; return true;
        }
        case 0x41c3: case 0x41c8: case 0x41ca: case 0x41d8: case 0x41de: {
            const bool add = mcu.pc == 0x41de, compare = mcu.pc == 0x41d8;
            const unsigned reg = mcu.pc == 0x41ca || add ? 4 : 6;
            const unsigned a = mcu.pc == 0x41c8 ? 0 : mcu.r[reg]&255;
            const unsigned b = mcu.pc == 0x41c3 ? 64 : compare ? 16 : mcu.r[6]&255;
            const unsigned wide = add ? a+b : a-b, value = wide&255;
            if (!compare) mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&128 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(wide&256 ? STATUS_C : 0)|((add ? ~(a^b)&(a^value) : (a^b)&(a^value))&128 ? STATUS_V : 0));
            mcu.pc += mcu.pc == 0x41c3 ? 3 : 2; return true;
        }
        case 0x41c6: mcu.pc = (mcu.sr&STATUS_C) ? 0x41c8 : 0x41d2; return true;
        case 0x41cc: case 0x41e0: mcu.pc = (mcu.sr&STATUS_N) ? uint16_t(mcu.pc+2) : 0x41e4; return true;
        case 0x41ce: mcu.r[4] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x41d0: mcu.pc = 0x41e4; return true;
        case 0x41d2:
            mcu.sr = uint16_t((mcu.sr&~STATUS_Z)|((MCU_Read(mcu,voice+162)&4) ? 0 : STATUS_Z)); mcu.pc += 4; return true;
        case 0x41d6: mcu.pc = (mcu.sr&STATUS_Z) ? 0x41d8 : 0x41e4; return true;
        case 0x41da: mcu.pc = (mcu.sr&STATUS_C) ? 0x41de : 0x41dc; return true;
        case 0x41dc: mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|16); nz(mcu.r[6],true); mcu.pc += 2; return true;
        case 0x41e2: mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|127); nz(mcu.r[4],true); mcu.pc += 2; return true;
        case 0x41e4: mcu.r[4] = uint16_t((mcu.r[4]<<8)|(mcu.r[4]>>8)); nz(mcu.r[4]); mcu.pc += 2; return true;
        case 0x41e6: mcu.sr &= ~STATUS_C; nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x41e8: mcu.pc = (mcu.sr&STATUS_N) ? 0x41ea : 0x41f2; return true;
        case 0x41ec: case 0x41f4: mcu.pc = (mcu.sr&STATUS_N) ? uint16_t(mcu.pc+2) : 0x41f9; return true;
        case 0x41fd: mcu.pc = (mcu.sr&STATUS_N) ? 0x41ff : 0x4202; return true;
        case 0x41ee: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x41f0: mcu.pc = 0x41f9; return true;
        case 0x41f6: case 0x41ff: case 0x4216:
            mcu.r[3] = mcu.pc == 0x4216 ? 0xff00 : 0x7fff; nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x41ea: case 0x41f2: case 0x41f9: case 0x4206: case 0x420e: case 0x4210: {
            const bool immediate = mcu.pc == 0x41f9 || mcu.pc == 0x4210;
            const unsigned a = mcu.r[3], b = immediate ? 255 : mcu.r[mcu.pc == 0x41ea || mcu.pc == 0x41f2 ? 4 : 3];
            const unsigned sum = a+b, value = sum&65535;
            mcu.r[3] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|(~(a^b)&(a^value)&32768 ? STATUS_V : 0));
            mcu.pc += immediate ? 4 : 2; return true;
        }
        case 0x4202: case 0x4219:
            mcu.r[3] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x4204: case 0x421b:
            mcu.r[3] = uint16_t((mcu.r[3]<<8)|(mcu.r[3]>>8)); nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x4208:
            if ((mcu.r[3]&1) || mcu.r[3] > 254) return false;
            mcu.r[6] = ReadWord(mcu,0x7612+mcu.r[3]); nz(mcu.r[6]); mcu.pc += 4; return true;
        case 0x420c: mcu.r[3] = mcu.r[6]; nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x4214: mcu.pc = (mcu.sr&STATUS_C) ? 0x4216 : 0x4219; return true;
        case 0x421d:
            if (mcu.r[3] > 255) return false;
            mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|MCU_Read(mcu,0x7714+mcu.r[3])); nz(mcu.r[6],true); mcu.pc += 4; return true;
        case 0x4221: MCU_Write(mcu,voice+105,mcu.r[6]&255); nz(mcu.r[6],true); mcu.pc += 3; return true;
        case 0x4224: mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|MCU_Read(mcu,voice+103)); nz(mcu.r[4],true); mcu.pc += 3; return true;
        case 0x422a: {
            const unsigned address = uint16_t(mcu.r[2]+19);
            if (address >= 0xe000) return false;
            mcu.r[5] = uint16_t((mcu.r[5]&0xff00)|MCU_Read(mcu,address)); nz(mcu.r[5],true); mcu.pc += 3; return true;
        }
        case 0x422d: case 0x4232: case 0x4234: case 0x4238: case 0x4240: case 0x4242: case 0x4244: case 0x4246: {
            const bool compare = mcu.pc == 0x4238 || mcu.pc == 0x4246;
            const bool add = mcu.pc == 0x4232 || mcu.pc == 0x4242 || mcu.pc == 0x4244;
            const unsigned reg = mcu.pc == 0x422d || mcu.pc == 0x4232 || mcu.pc == 0x4240 || mcu.pc == 0x4242 ? 5 : 4;
            const unsigned a = mcu.pc == 0x4240 ? 0 : mcu.r[reg]&255;
            const unsigned b = mcu.pc == 0x422d ? 64 : mcu.r[compare ? 6 : 5]&255;
            const unsigned wide = add ? a+b : a-b, value = wide&255;
            if (!compare) mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&128 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(wide&256 ? STATUS_C : 0)|((add ? ~(a^b)&(a^value) : (a^b)&(a^value))&128 ? STATUS_V : 0));
            mcu.pc += mcu.pc == 0x422d ? 3 : 2; return true;
        }
        case 0x4230: mcu.pc = (mcu.sr&STATUS_N) ? 0x4240 : 0x4232; return true;
        case 0x4236: mcu.pc = (mcu.sr&STATUS_N) ? 0x424e : 0x4238; return true;
        case 0x423a: case 0x4248: mcu.pc = (mcu.sr&STATUS_C) ? 0x4250 : uint16_t(mcu.pc+2); return true;
        case 0x423c: case 0x424a:
            mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|(mcu.r[6]&255)); nz(mcu.r[4],true); mcu.pc += 2; return true;
        case 0x423e: case 0x424c: mcu.pc = 0x4250; return true;
        case 0x424e: mcu.r[4] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x4250: MCU_Write(mcu,voice+104,mcu.r[4]&255); nz(mcu.r[4],true); mcu.pc += 3; return true;
        case 0x4253: mcu.ep = MCU_Read(mcu,voice+153); mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x4257: mcu.r[5] = ReadWord(mcu,voice+158); nz(mcu.r[5]); mcu.pc += 4; return true;
        default: return false;
    }
}

inline bool TryStepFilterCurve(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    const bool romPage = mcu.pc == 0x426b || mcu.pc == 0x426f || mcu.pc == 0x4271 || mcu.pc == 0x4273;
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp != (romPage ? 3 : 0) || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x425b: case 0x4261: case 0x4276:
            mcu.r[mcu.pc == 0x425b ? 4 : mcu.pc == 0x4261 ? 2 : 3] = 0;
            mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x425d:
            if (mcu.r[1] >= 24) return false;
            mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|MCU_Read(mcu,0xc8fc+mcu.r[1])); nz(mcu.r[4],true); mcu.pc += 4; return true;
        case 0x4263: case 0x4278: {
            const bool first = mcu.pc == 0x4263;
            const unsigned reg = first ? 2 : 3, address = uint16_t(mcu.r[5]+(first ? 57 : 59));
            if (mcu.ep > 4 || (!mcu.ep && address >= 0xe000)) return false;
            mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|MCU_Read(mcu,(unsigned(mcu.ep)<<16)|address));
            nz(mcu.r[reg],true); mcu.pc += 3; return true;
        }
        case 0x4266: case 0x426f: {
            const unsigned a = mcu.r[2], b = mcu.r[mcu.pc == 0x4266 ? 2 : 4], sum = a+b, value = sum&65535;
            mcu.r[2] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|(~(a^b)&(a^value)&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x4268: case 0x4273:
            mcu.dp = mcu.pc == 0x4268 ? 3 : 0; mcu.ex_ignore = 1; mcu.pc += 3; return true;
        case 0x426b:
            if ((mcu.r[2]&1) || mcu.r[2] > 510) return false;
            mcu.r[2] = MCU_Read16(mcu,0x30000|uint16_t(0xdcf2+mcu.r[2])); nz(mcu.r[2]); mcu.pc += 4; return true;
        case 0x4271:
            mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|MCU_Read(mcu,0x30000|mcu.r[2])); nz(mcu.r[4],true); mcu.pc += 2; return true;
        case 0x427b: case 0x4282: case 0x4284: case 0x428a: case 0x4291: {
            const bool immediate = mcu.pc == 0x427b || mcu.pc == 0x428a;
            const unsigned reg = mcu.pc == 0x427b || mcu.pc == 0x4282 ? 3 : 4;
            const unsigned a = immediate ? mcu.r[reg]&255 : 0, b = immediate ? (reg == 3 ? 64 : 128) : mcu.r[reg]&255;
            const unsigned value = (a-b)&255;
            mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&128 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(a < b ? STATUS_C : 0)|((a^b)&(a^value)&128 ? STATUS_V : 0));
            mcu.pc += immediate ? 3 : 2; return true;
        }
        case 0x427e: mcu.pc = (mcu.sr&STATUS_Z) ? 0x42ba : 0x4280; return true;
        case 0x4280: mcu.pc = (mcu.sr&STATUS_C) ? 0x4282 : 0x428a; return true;
        case 0x4286: mcu.pc = (mcu.sr&STATUS_Z) ? 0x4288 : 0x428a; return true;
        case 0x4288: mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|255); nz(mcu.r[4],true); mcu.pc += 2; return true;
        case 0x428d: mcu.pc = (mcu.sr&STATUS_Z) ? 0x42ba : 0x428f; return true;
        case 0x428f: mcu.pc = (mcu.sr&STATUS_C) ? 0x4291 : 0x42a2; return true;
        case 0x4293: case 0x42a2: {
            if (mcu.r[3] > 255) return false;
            const unsigned product = (mcu.r[4]&255)*MCU_Read(mcu,0x679a+mcu.r[3]);
            mcu.r[4] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&32768 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += 4; return true;
        }
        case 0x4297: case 0x42a6: case 0x42af: {
            const unsigned reg = mcu.pc == 0x42af ? 3 : 4, a = mcu.r[reg], sum = a+a, value = sum&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|((a^value)&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x4299: case 0x42a8:
            mcu.r[4] = uint16_t((mcu.r[4]<<8)|(mcu.r[4]>>8)); nz(mcu.r[4]); mcu.pc += 2; return true;
        case 0x429b: case 0x42aa: mcu.r[3] = 128; nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x429e: case 0x42ad: {
            const bool add = mcu.pc == 0x42ad;
            const unsigned a = mcu.r[3]&255, b = mcu.r[4]&255, wide = add ? a+b : a-b, value = wide&255;
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&128 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(wide&256 ? STATUS_C : 0)|((add ? ~(a^b)&(a^value) : (a^b)&(a^value))&128 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x42a0: mcu.pc = 0x42af; return true;
        case 0x42b1:
            if ((mcu.r[3]&1) || mcu.r[3] > 510) return false;
            mcu.r[3] = ReadWord(mcu,0x67c6+mcu.r[3]); nz(mcu.r[3]); mcu.pc += 4; return true;
        case 0x42b5: MCU_Write16(mcu,voice-44,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x42b8: mcu.pc = 0x42bf; return true;
        case 0x42ba: MCU_Write16(mcu,voice-44,256); nz(256); mcu.pc += 5; return true;
        default: return false;
    }
}

inline bool TryStepSecondFilterCurve(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    const bool romPage = mcu.pc == 0x42cf || mcu.pc == 0x42d3 || mcu.pc == 0x42d5 || mcu.pc == 0x42d7;
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp != (romPage ? 3 : 0) || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x42bf: case 0x42c5: case 0x42da:
            mcu.r[mcu.pc == 0x42bf ? 4 : mcu.pc == 0x42c5 ? 2 : 3] = 0;
            mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x42c1:
            if (mcu.r[1] >= 24) return false;
            mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|MCU_Read(mcu,0xc8fc+mcu.r[1])); nz(mcu.r[4],true); mcu.pc += 4; return true;
        case 0x42c7: case 0x42dc: {
            const bool first = mcu.pc == 0x42c7;
            const unsigned reg = first ? 2 : 3, address = uint16_t(mcu.r[5]+(first ? 58 : 60));
            if (mcu.ep > 4 || (!mcu.ep && address >= 0xe000)) return false;
            mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|MCU_Read(mcu,(unsigned(mcu.ep)<<16)|address));
            nz(mcu.r[reg],true); mcu.pc += 3; return true;
        }
        case 0x42ca: case 0x42d3: {
            const unsigned a = mcu.r[2], b = mcu.r[mcu.pc == 0x42ca ? 2 : 4], sum = a+b, value = sum&65535;
            mcu.r[2] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|(~(a^b)&(a^value)&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x42cc: case 0x42d7:
            mcu.dp = mcu.pc == 0x42cc ? 3 : 0; mcu.ex_ignore = 1; mcu.pc += 3; return true;
        case 0x42cf:
            if ((mcu.r[2]&1) || mcu.r[2] > 510) return false;
            mcu.r[2] = MCU_Read16(mcu,0x30000|uint16_t(0xdd12+mcu.r[2])); nz(mcu.r[2]); mcu.pc += 4; return true;
        case 0x42d5:
            mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|MCU_Read(mcu,0x30000|mcu.r[2])); nz(mcu.r[4],true); mcu.pc += 2; return true;
        case 0x42df: case 0x42e6: case 0x42e8: case 0x42ee: case 0x42f5: {
            const bool immediate = mcu.pc == 0x42df || mcu.pc == 0x42ee;
            const unsigned reg = mcu.pc == 0x42df || mcu.pc == 0x42e6 ? 3 : 4;
            const unsigned a = immediate ? mcu.r[reg]&255 : 0, b = immediate ? (reg == 3 ? 64 : 128) : mcu.r[reg]&255;
            const unsigned value = (a-b)&255;
            mcu.r[reg] = uint16_t((mcu.r[reg]&0xff00)|value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&128 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(a < b ? STATUS_C : 0)|((a^b)&(a^value)&128 ? STATUS_V : 0));
            mcu.pc += immediate ? 3 : 2; return true;
        }
        case 0x42e2: mcu.pc = (mcu.sr&STATUS_Z) ? 0x4322 : 0x42e4; return true;
        case 0x42e4: mcu.pc = (mcu.sr&STATUS_C) ? 0x42e6 : 0x42ee; return true;
        case 0x42ea: mcu.pc = (mcu.sr&STATUS_Z) ? 0x42ec : 0x42ee; return true;
        case 0x42ec: mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|255); nz(mcu.r[4],true); mcu.pc += 2; return true;
        case 0x42f1: mcu.pc = (mcu.sr&STATUS_Z) ? 0x4322 : 0x42f3; return true;
        case 0x42f3: mcu.pc = (mcu.sr&STATUS_C) ? 0x42f5 : 0x4306; return true;
        case 0x42f7: case 0x4306: {
            if (mcu.r[3] > 255) return false;
            const unsigned product = (mcu.r[4]&255)*MCU_Read(mcu,0x679a+mcu.r[3]);
            mcu.r[4] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&32768 ? STATUS_N : 0)|(!product ? STATUS_Z : 0));
            mcu.pc += 4; return true;
        }
        case 0x42fb: case 0x430a: case 0x4317: {
            const unsigned reg = mcu.pc == 0x4317 ? 3 : 4, a = mcu.r[reg], sum = a+a, value = sum&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|((a^value)&32768 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x42fd: case 0x430c:
            mcu.r[4] = uint16_t((mcu.r[4]<<8)|(mcu.r[4]>>8)); nz(mcu.r[4]); mcu.pc += 2; return true;
        case 0x42ff: case 0x430e: mcu.r[3] = 128; nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x4302: case 0x4311: {
            const bool add = mcu.pc == 0x4311;
            const unsigned a = mcu.r[3]&255, b = mcu.r[4]&255, wide = add ? a+b : a-b, value = wide&255;
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&128 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(wide&256 ? STATUS_C : 0)|((add ? ~(a^b)&(a^value) : (a^b)&(a^value))&128 ? STATUS_V : 0));
            mcu.pc += 2; return true;
        }
        case 0x4304: mcu.pc = 0x4313; return true;
        case 0x4319:
            if ((mcu.r[3]&1) || mcu.r[3] > 510) return false;
            mcu.r[3] = ReadWord(mcu,0x67c6+mcu.r[3]); nz(mcu.r[3]); mcu.pc += 4; return true;
        case 0x431d: MCU_Write16(mcu,voice-42,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x4320: mcu.pc = 0x4327; return true;
        case 0x4322: MCU_Write16(mcu,voice-42,256); nz(256); mcu.pc += 5; return true;
        case 0x4313: mcu.r[3] &= 255; nz(mcu.r[3]); mcu.pc += 4; return true;
        default: return false;
    }
}

inline bool TryStepFilterControllerScale(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x4327: case 0x4367: mcu.r[2] = mcu.pc == 0x4327 ? 127 : 31; nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x432e: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x4330: {
            const unsigned address = uint16_t(mcu.r[5]+62);
            if (mcu.ep > 4 || (!mcu.ep && address >= 0xe000)) return false;
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|MCU_Read(mcu,(unsigned(mcu.ep)<<16)|address)); nz(mcu.r[3],true); mcu.pc += 3; return true;
        }
        case 0x432a: case 0x4333: case 0x433a: case 0x4340: case 0x434d: case 0x4360: case 0x4362: {
            const bool byte = mcu.pc == 0x432a || mcu.pc == 0x4333 || mcu.pc == 0x433a;
            const bool compare = mcu.pc == 0x4340 || mcu.pc == 0x4362;
            const unsigned reg = mcu.pc == 0x432a || mcu.pc == 0x4340 ? 2 : mcu.pc == 0x4360 || mcu.pc == 0x4362 ? 4 : 3;
            if (mcu.pc == 0x432a && mcu.r[1] >= 24) return false;
            const unsigned mask = byte ? 255 : 65535, sign = byte ? 128 : 32768;
            const unsigned a = mcu.pc == 0x433a ? 0 : mcu.r[reg]&mask;
            const unsigned b = (mcu.pc == 0x432a ? MCU_Read(mcu,0xc95c+mcu.r[1]) : mcu.pc == 0x4333 ? 64 : mcu.pc == 0x433a ? mcu.r[3] : mcu.pc == 0x4340 ? 0x1f41 : mcu.pc == 0x4362 ? 32 : mcu.r[2])&mask;
            const unsigned value = (a-b)&mask;
            if (!compare) mcu.r[reg] = uint16_t((byte ? mcu.r[reg]&0xff00 : 0)|value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&sign ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(a < b ? STATUS_C : 0)|((a^b)&(a^value)&sign ? STATUS_V : 0));
            mcu.pc += mcu.pc == 0x432a ? 4 : mcu.pc == 0x4333 || compare ? 3 : 2; return true;
        }
        case 0x4336: mcu.pc = (mcu.sr&STATUS_Z) ? 0x4376 : 0x4338; return true;
        case 0x4338: mcu.pc = (mcu.sr&STATUS_C) ? 0x433a : 0x4359; return true;
        case 0x4343: mcu.pc = (mcu.sr&STATUS_C) ? 0x434a : 0x4345; return true;
        case 0x4365: mcu.pc = (mcu.sr&STATUS_C) ? 0x4371 : 0x4367; return true;
        case 0x433c: case 0x435c: {
            if (mcu.r[3] > 255) return false;
            const unsigned product = (mcu.r[2]&255)*MCU_Read(mcu,0x679a+mcu.r[3]);
            mcu.r[2] = uint16_t(product); mcu.sr &= ~STATUS_C; nz(product);
            mcu.pc += 4; return true;
        }
        case 0x4345: case 0x434a: case 0x436a: case 0x4371: case 0x4376:
            mcu.r[3] = mcu.pc == 0x4345 ? 4 : mcu.pc == 0x434a ? 0x1fc0 : mcu.pc == 0x436a ? 0xc000 : mcu.pc == 0x4371 ? 65535 : 256;
            nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x4359: mcu.r[4] = 0x1fc0; nz(mcu.r[4]); mcu.pc += 3; return true;
        case 0x4348: case 0x4357: case 0x436f: case 0x4374: mcu.pc = 0x4379; return true;
        case 0x434f: mcu.r[2] = mcu.r[3]; nz(mcu.r[2]); mcu.pc += 2; return true;
        case 0x4355: mcu.r[3] = mcu.r[2]; nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x4351: {
            const uint32_t product = uint32_t(mcu.r[2])*0x810;
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0)); mcu.pc += 4; return true;
        }
        case 0x436d: {
            if (!mcu.r[4]) return false;
            const uint32_t dividend = (uint32_t(mcu.r[2])<<16)|mcu.r[3], quotient = dividend/mcu.r[4];
            if (quotient > 65535) mcu.sr = uint16_t((mcu.sr&~15)|STATUS_V);
            else {
                mcu.r[2] = uint16_t(dividend%mcu.r[4]); mcu.r[3] = uint16_t(quotient); mcu.sr &= ~STATUS_C; nz(quotient);
            }
            mcu.pc += 2; return true;
        }
        case 0x4379: MCU_Write16(mcu,voice-40,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        default: return false;
    }
}

inline bool TryStepSecondFilterControllerScale(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x437c: case 0x43bc: mcu.r[2] = mcu.pc == 0x437c ? 127 : 31; nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x4383: mcu.r[3] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x4385: {
            const unsigned address = uint16_t(mcu.r[5]+63);
            if (mcu.ep > 4 || (!mcu.ep && address >= 0xe000)) return false;
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|MCU_Read(mcu,(unsigned(mcu.ep)<<16)|address)); nz(mcu.r[3],true); mcu.pc += 3; return true;
        }
        case 0x437f: case 0x4388: case 0x438f: case 0x4395: case 0x43a2: case 0x43b5: case 0x43b7: {
            const bool byte = mcu.pc == 0x437f || mcu.pc == 0x4388 || mcu.pc == 0x438f;
            const bool compare = mcu.pc == 0x4395 || mcu.pc == 0x43b7;
            const unsigned reg = mcu.pc == 0x437f || mcu.pc == 0x4395 ? 2 : mcu.pc == 0x43b5 || mcu.pc == 0x43b7 ? 4 : 3;
            if (mcu.pc == 0x437f && mcu.r[1] >= 24) return false;
            const unsigned mask = byte ? 255 : 65535, sign = byte ? 128 : 32768;
            const unsigned a = mcu.pc == 0x438f ? 0 : mcu.r[reg]&mask;
            const unsigned b = (mcu.pc == 0x437f ? MCU_Read(mcu,0xc95c+mcu.r[1]) : mcu.pc == 0x4388 ? 64 : mcu.pc == 0x438f ? mcu.r[3] : mcu.pc == 0x4395 ? 0x1f41 : mcu.pc == 0x43b7 ? 32 : mcu.r[2])&mask;
            const unsigned value = (a-b)&mask;
            if (!compare) mcu.r[reg] = uint16_t((byte ? mcu.r[reg]&0xff00 : 0)|value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&sign ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(a < b ? STATUS_C : 0)|((a^b)&(a^value)&sign ? STATUS_V : 0));
            mcu.pc += mcu.pc == 0x437f ? 4 : mcu.pc == 0x4388 || compare ? 3 : 2; return true;
        }
        case 0x438b: mcu.pc = (mcu.sr&STATUS_Z) ? 0x43cb : 0x438d; return true;
        case 0x438d: mcu.pc = (mcu.sr&STATUS_C) ? 0x438f : 0x43ae; return true;
        case 0x4398: mcu.pc = (mcu.sr&STATUS_C) ? 0x439f : 0x439a; return true;
        case 0x43ba: mcu.pc = (mcu.sr&STATUS_C) ? 0x43c6 : 0x43bc; return true;
        case 0x4391: case 0x43b1: {
            if (mcu.r[3] > 255) return false;
            const unsigned product = (mcu.r[2]&255)*MCU_Read(mcu,0x679a+mcu.r[3]);
            mcu.r[2] = uint16_t(product); mcu.sr &= ~STATUS_C; nz(product);
            mcu.pc += 4; return true;
        }
        case 0x439a: case 0x439f: case 0x43bf: case 0x43c6: case 0x43cb:
            mcu.r[3] = mcu.pc == 0x439a ? 4 : mcu.pc == 0x439f ? 0x1fc0 : mcu.pc == 0x43bf ? 0xc000 : mcu.pc == 0x43c6 ? 65535 : 256;
            nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x43ae: mcu.r[4] = 0x1fc0; nz(mcu.r[4]); mcu.pc += 3; return true;
        case 0x439d: case 0x43ac: case 0x43c4: case 0x43c9: mcu.pc = 0x43ce; return true;
        case 0x43a4: mcu.r[2] = mcu.r[3]; nz(mcu.r[2]); mcu.pc += 2; return true;
        case 0x43aa: mcu.r[3] = mcu.r[2]; nz(mcu.r[3]); mcu.pc += 2; return true;
        case 0x43a6: {
            const uint32_t product = uint32_t(mcu.r[2])*0x810;
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0)); mcu.pc += 4; return true;
        }
        case 0x43c2: {
            if (!mcu.r[4]) return false;
            const uint32_t dividend = (uint32_t(mcu.r[2])<<16)|mcu.r[3], quotient = dividend/mcu.r[4];
            if (quotient > 65535) mcu.sr = uint16_t((mcu.sr&~15)|STATUS_V);
            else {
                mcu.r[2] = uint16_t(dividend%mcu.r[4]); mcu.r[3] = uint16_t(quotient); mcu.sr &= ~STATUS_C; nz(quotient);
            }
            mcu.pc += 2; return true;
        }
        case 0x43ce: MCU_Write16(mcu,voice-38,mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        default: return false;
    }
}

inline bool TryStepFilterSetup(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    if (mcu.pc >= 0x43d1 && mcu.pc <= 0x4400) {
        const unsigned index = (mcu.pc-0x43d1)/10, step = (mcu.pc-0x43d1)%10;
        if (index > 4) return false;
        if (step == 0) {
            const unsigned address = uint16_t(mcu.r[5]+50+index);
            if (mcu.ep > 4 || (!mcu.ep && address >= 0xe000)) return false;
            mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|MCU_Read(mcu,(unsigned(mcu.ep)<<16)|address));
            nz(mcu.r[2],true); mcu.pc += 3; return true;
        }
        if (step == 3) { mcu.r[2] &= 127; nz(mcu.r[2]); mcu.pc += 4; return true; }
        if (step == 7) { MCU_Write(mcu,voice+74+index,mcu.r[2]&255); nz(mcu.r[2],true); mcu.pc += 3; return true; }
        return false;
    }
    switch (mcu.pc) {
        case 0x4403: case 0x442b: {
            if (mcu.tp || (mcu.r[7]&1) || mcu.r[7] < 0x8002 || mcu.r[7] > 0xe000) return false;
            const bool first = mcu.pc == 0x4403;
            mcu.pc += first ? 3 : 2; MCU_PushStack(mcu,mcu.pc); mcu.pc = first ? 0x4494 : 0x44a3; return true;
        }
        case 0x4406: case 0x4409:
            MCU_Write16(mcu,voice+(mcu.pc == 0x4406 ? 20 : 10),0); mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 3; return true;
        case 0x440c: case 0x4412:
            mcu.r[2] = ReadWord(mcu,voice+(mcu.pc == 0x440c ? 36 : 38)); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x440f: case 0x4417:
            MCU_Write16(mcu,voice-(mcu.pc == 0x440f ? 22 : 24),mcu.r[2]); nz(mcu.r[2]); mcu.pc += 3; return true;
        case 0x4415: mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|0xba); nz(mcu.r[2],true); mcu.pc += 2; return true;
        case 0x441a: case 0x442d:
            mcu.r[6] = ReadWord(mcu,mcu.pc == 0x441a ? 0xac5a : 0xac5c); nz(mcu.r[6]); mcu.pc += 4; return true;
        case 0x441e: case 0x4431:
            MCU_Write16(mcu,mcu.pc == 0x441e ? 0xac5c : 0xac5a,mcu.r[6]); nz(mcu.r[6]); mcu.pc += 4; return true;
        case 0x4422: MCU_Write16(mcu,0xac5a,1); nz(1); mcu.pc += 6; return true;
        case 0x4428: case 0x4435: mcu.r[1] = ReadWord(mcu,voice-2); nz(mcu.r[1]); mcu.pc += 3; return true;
        case 0x4438: {
            if (mcu.r[1] >= 24) return false;
            const unsigned value = MCU_Read(mcu,0xcaf4+mcu.r[1]);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&128 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)); mcu.pc += 5; return true;
        }
        case 0x443d: mcu.pc = (mcu.sr&STATUS_Z) ? 0x4440 : 0x56d9; return true;
        case 0x4440: mcu.pc = 0x4858; return true;
        default: return false;
    }
}

inline bool TryStepVoiceBaseValue(mcu_t& mcu)
{
    const unsigned voice = mcu.r[0];
    const bool romPage = mcu.pc == 0x48ed || mcu.pc == 0x48f1 || mcu.pc == 0x48f3 || mcu.pc == 0x48f5;
    if (!mcu.native_v121_enabled || mcu.cp || mcu.dp != (romPage ? 3 : 0) || (mcu.sr&STATUS_T)
        || voice < 0xacde || voice > 0xc7a4 || (voice-0xacde)%0x12a) return false;
    auto nz = [&](unsigned value, bool byte = false) {
        value &= byte ? 255 : 65535;
        mcu.sr = uint16_t((mcu.sr&~14)|(value&(byte ? 128 : 32768) ? STATUS_N : 0)|(!value ? STATUS_Z : 0));
    };
    switch (mcu.pc) {
        case 0x4858:
            MCU_ControlRegisterWrite(mcu,0,MCU_Operand_Size::WORD,mcu.sr|0x700); mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x485c: mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|MCU_Read(mcu,voice+40)); nz(mcu.r[6],true); mcu.pc += 3; return true;
        case 0x485f: MCU_Write(mcu,0xc8b2,mcu.r[6]&255); nz(mcu.r[6],true); mcu.pc += 4; return true;
        case 0x4863: mcu.r[6] = ReadWord(mcu,voice+60); nz(mcu.r[6]); mcu.pc += 3; return true;
        case 0x4866: MCU_Write16(mcu,0xc8ae,mcu.r[6]); nz(mcu.r[6]); mcu.pc += 4; return true;
        case 0x486a: mcu.r[4] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x486c:
            if (mcu.r[1] >= 24) return false;
            mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|MCU_Read(mcu,0xc98c+mcu.r[1])); nz(mcu.r[4],true); mcu.pc += 4; return true;
        case 0x4870: {
            const uint32_t product = uint32_t(mcu.r[4])*1000;
            mcu.r[4] = uint16_t(product>>16); mcu.r[5] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0)); mcu.pc += 4; return true;
        }
        case 0x4874: mcu.r[2] = mcu.r[1]; nz(mcu.r[2]); mcu.pc += 2; return true;
        case 0x4876: case 0x4878: {
            const bool table = mcu.pc == 0x4878;
            if (table && ((mcu.r[2]&1) || mcu.r[2] > 46)) return false;
            const unsigned reg = table ? 5 : 2, a = mcu.r[reg], b = table ? ReadWord(mcu,0xc9a4+mcu.r[2]) : a;
            const unsigned sum = a+b, value = sum&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|(~(a^b)&(a^value)&32768 ? STATUS_V : 0));
            mcu.pc += table ? 4 : 2; return true;
        }
        case 0x487c: {
            const unsigned a = mcu.r[4]&255, carry = (mcu.sr&STATUS_C) ? 1 : 0, sum = a+carry, value = sum&255;
            const bool zero = (mcu.sr&STATUS_Z) != 0;
            mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&128 ? STATUS_N : 0)|(!value && zero ? STATUS_Z : 0)
                |(sum > 255 ? STATUS_C : 0)|(a == 127 && carry ? STATUS_V : 0)); mcu.pc += 3; return true;
        }
        case 0x487f: MCU_Write(mcu,voice+40,mcu.r[4]&255); nz(mcu.r[4],true); mcu.pc += 3; return true;
        case 0x4882: MCU_Write16(mcu,voice+60,mcu.r[5]); nz(mcu.r[5]); mcu.pc += 3; return true;
        case 0x4885: mcu.ep = MCU_Read(mcu,voice+154); mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x4889: mcu.r[5] = ReadWord(mcu,voice+160); nz(mcu.r[5]); mcu.pc += 4; return true;
        case 0x488d: mcu.r[2] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x488f: case 0x4896: case 0x48b3: {
            const bool byte = mcu.pc == 0x488f;
            const unsigned offset = byte ? 11 : mcu.pc == 0x4896 ? 12 : 14;
            const unsigned address = uint16_t(mcu.r[5]+offset), reg = byte ? 2 : 6;
            if (mcu.ep > 4 || (!mcu.ep && address >= 0xe000) || (!byte && (address&1))) return false;
            const unsigned full = (unsigned(mcu.ep)<<16)|address;
            mcu.r[reg] = byte ? uint16_t((mcu.r[reg]&0xff00)|MCU_Read(mcu,full)) : MCU_Read16(mcu,full);
            nz(mcu.r[reg],byte); mcu.pc += 3; return true;
        }
        case 0x4892: {
            const uint32_t product = uint32_t(mcu.r[2])*1000;
            mcu.r[2] = uint16_t(product>>16); mcu.r[3] = uint16_t(product);
            mcu.sr = uint16_t((mcu.sr&~15)|(product&0x80000000 ? STATUS_N : 0)|(!product ? STATUS_Z : 0)); mcu.pc += 4; return true;
        }
        case 0x4899: case 0x48b6: case 0x489f: case 0x48bc: case 0x48a1: case 0x48be: case 0x48a8: case 0x48c5: {
            const bool immediate = mcu.pc == 0x4899 || mcu.pc == 0x48b6;
            const bool negate = mcu.pc == 0x489f || mcu.pc == 0x48bc;
            const bool add = mcu.pc == 0x48a1 || mcu.pc == 0x48be;
            const unsigned reg = immediate || negate ? 6 : 3, a = negate ? 0 : mcu.r[reg], b = immediate ? 1024 : mcu.r[6];
            const unsigned wide = add ? a+b : a-b, value = wide&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(wide&65536 ? STATUS_C : 0)|((add ? ~(a^b)&(a^value) : (a^b)&(a^value))&32768 ? STATUS_V : 0));
            mcu.pc += immediate ? 4 : 2; return true;
        }
        case 0x489d: mcu.pc = (mcu.sr&STATUS_C) ? 0x489f : 0x48a8; return true;
        case 0x48ba: mcu.pc = (mcu.sr&STATUS_C) ? 0x48bc : 0x48c5; return true;
        case 0x48a6: mcu.pc = 0x48ad; return true;
        case 0x48c3: mcu.pc = 0x48ca; return true;
        case 0x48a3: case 0x48c0: case 0x48aa: case 0x48c7: {
            const bool add = mcu.pc == 0x48a3 || mcu.pc == 0x48c0;
            const unsigned a = mcu.r[2]&255, carry = (mcu.sr&STATUS_C) ? 1 : 0;
            const unsigned wide = add ? a+carry : a-carry, value = wide&255;
            const bool zero = (mcu.sr&STATUS_Z) != 0;
            mcu.r[2] = uint16_t((mcu.r[2]&0xff00)|value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&128 ? STATUS_N : 0)|(!value && (!add || zero) ? STATUS_Z : 0)
                |(wide&256 ? STATUS_C : 0)|(carry && a == (add ? 127 : 128) ? STATUS_V : 0)); mcu.pc += 3; return true;
        }
        case 0x48ad: case 0x48ca:
            MCU_Write(mcu,voice+(mcu.pc == 0x48ad ? 41 : 42),mcu.r[2]&255); nz(mcu.r[2],true); mcu.pc += 3; return true;
        case 0x48b0: case 0x48cd:
            MCU_Write16(mcu,voice+(mcu.pc == 0x48b0 ? 62 : 64),mcu.r[3]); nz(mcu.r[3]); mcu.pc += 3; return true;
        case 0x48d0: mcu.ep = MCU_Read(mcu,voice+152); mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x48d4: mcu.r[5] = ReadWord(mcu,voice+156); nz(mcu.r[5]); mcu.pc += 4; return true;
        case 0x48d8: case 0x48df:
            mcu.r[mcu.pc == 0x48d8 ? 3 : 6] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x48da: {
            const unsigned address = uint16_t(mcu.r[5]+19);
            if (mcu.ep > 4 || (!mcu.ep && address >= 0xe000)) return false;
            mcu.r[3] = uint16_t((mcu.r[3]&0xff00)|MCU_Read(mcu,(unsigned(mcu.ep)<<16)|address)); nz(mcu.r[3],true); mcu.pc += 3; return true;
        }
        case 0x48dd: mcu.pc = (mcu.sr&STATUS_Z) ? 0x491f : 0x48df; return true;
        case 0x48e1:
            if (mcu.r[1] >= 24) return false;
            mcu.r[6] = uint16_t((mcu.r[6]&0xff00)|MCU_Read(mcu,0xc98c+mcu.r[1])); nz(mcu.r[6],true); mcu.pc += 4; return true;
        case 0x48e5: case 0x48e7: case 0x48f1: {
            const unsigned reg = mcu.pc == 0x48e5 ? 6 : 3, a = mcu.r[reg], b = mcu.r[mcu.pc == 0x48f1 ? 6 : reg];
            const unsigned sum = a+b, value = sum&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(sum > 65535 ? STATUS_C : 0)|(~(a^b)&(a^value)&32768 ? STATUS_V : 0)); mcu.pc += 2; return true;
        }
        // These immediate LDC.W instructions are four bytes, unlike LDC.B.
        case 0x48e9: case 0x48f5:
            mcu.dp = mcu.pc == 0x48e9 ? 3 : 0; mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x48ed:
            if ((mcu.r[3]&1) || mcu.r[3] > 510) return false;
            mcu.r[3] = MCU_Read16(mcu,0x30000|uint16_t(0xdd32+mcu.r[3])); nz(mcu.r[3]); mcu.pc += 4; return true;
        case 0x48f3:
            if (mcu.r[3]&1) return false;
            mcu.r[6] = MCU_Read16(mcu,0x30000|mcu.r[3]); nz(mcu.r[6]); mcu.pc += 2; return true;
        case 0x48f9: mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|MCU_Read(mcu,voice+40)); nz(mcu.r[4],true); mcu.pc += 3; return true;
        case 0x48fc: mcu.r[5] = ReadWord(mcu,voice+60); nz(mcu.r[5]); mcu.pc += 3; return true;
        case 0x48ff: case 0x4905: case 0x4907: case 0x4914: {
            const bool immediate = mcu.pc == 0x48ff, negate = mcu.pc == 0x4905, add = mcu.pc == 0x4914;
            const unsigned reg = immediate || negate ? 6 : 5, a = negate ? 0 : mcu.r[reg], b = immediate ? 32768 : mcu.r[6];
            const unsigned wide = add ? a+b : a-b, value = wide&65535;
            mcu.r[reg] = uint16_t(value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&32768 ? STATUS_N : 0)|(!value ? STATUS_Z : 0)
                |(wide&65536 ? STATUS_C : 0)|((add ? ~(a^b)&(a^value) : (a^b)&(a^value))&32768 ? STATUS_V : 0));
            mcu.pc += immediate ? 4 : 2; return true;
        }
        case 0x4903: mcu.pc = (mcu.sr&STATUS_C) ? 0x4905 : 0x4914; return true;
        case 0x4909: case 0x4916: {
            const bool add = mcu.pc == 0x4916;
            const unsigned a = mcu.r[4]&255, carry = (mcu.sr&STATUS_C) ? 1 : 0;
            const unsigned wide = add ? a+carry : a-carry, value = wide&255;
            const bool zero = (mcu.sr&STATUS_Z) != 0;
            mcu.r[4] = uint16_t((mcu.r[4]&0xff00)|value);
            mcu.sr = uint16_t((mcu.sr&~15)|(value&128 ? STATUS_N : 0)|(!value && (!add || zero) ? STATUS_Z : 0)
                |(wide&256 ? STATUS_C : 0)|(carry && a == (add ? 127 : 128) ? STATUS_V : 0)); mcu.pc += 3; return true;
        }
        case 0x490c: mcu.pc = (mcu.sr&STATUS_N) ? 0x490e : 0x4919; return true;
        case 0x490e: mcu.r[4] &= 0xff00; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x4910: mcu.r[5] = 0; mcu.sr = uint16_t((mcu.sr&~15)|STATUS_Z); mcu.pc += 2; return true;
        case 0x4912: mcu.pc = 0x4919; return true;
        case 0x4919: MCU_Write(mcu,voice+40,mcu.r[4]&255); nz(mcu.r[4],true); mcu.pc += 3; return true;
        case 0x491c: MCU_Write16(mcu,voice+60,mcu.r[5]); nz(mcu.r[5]); mcu.pc += 3; return true;
        case 0x491f: mcu.ep = MCU_Read(mcu,voice+153); mcu.ex_ignore = 1; mcu.pc += 4; return true;
        case 0x4923: mcu.r[5] = ReadWord(mcu,voice+158); nz(mcu.r[5]); mcu.pc += 4; return true;
        default: return false;
    }
}

} // namespace mcu_native
