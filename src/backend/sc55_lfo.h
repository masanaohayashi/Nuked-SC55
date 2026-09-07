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
#include <array>
#include <algorithm>
#include "sc55_patch.h"
#include "sc55_modulation_tables.h"

namespace sc55
{

constexpr double kLfoPi = 3.1415926535897932384626433832795;

// LFO の正弦表（rom1[0x7412]、130 バイト）。
// 129 エントリすべてが round(255 * sin(pi * i / 128)) と完全に一致するので式で作る。
inline uint8_t LfoSine (int index)
{
    return (uint8_t) std::lround (255.0 * std::sin (kLfoPi * index / 128.0));
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
template<class Lookup>
inline int16_t LfoSineStep (uint16_t& phase, uint16_t increment,Lookup&& lookup)
{
    phase = (uint16_t) (phase + increment);

    // 0x8000 を折り目にして畳む。
    const uint16_t folded = phase >= 0x8000 ? (uint16_t) (phase - 0x8000)
                                            : (uint16_t) (0x8000 - phase);
    const uint32_t index = (folded >> 8) & 0xff;
    const uint32_t fraction = folded & 0xff;

    const int32_t low = lookup ((int) index);
    const int32_t difference = (int32_t) lookup ((int) index + 1) - low;

    uint16_t value = difference < 0 ? (uint16_t) ((low << 8) - (-difference) * fraction)
                                    : (uint16_t) ((low << 8) + difference * fraction);
    value = (uint16_t) (value >> 1);

    return phase > 0x8000 ? (int16_t) -(int16_t) value : (int16_t) value;
}

inline int16_t LfoSineStep(uint16_t& phase,uint16_t increment)
{
    return LfoSineStep(phase,increment,LfoSine);
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

// 遅延と立ち上がりのランプ（00:3b2c-00:3b80）。
//
// 2 段のカウンタを順に 0xffff まで進める。第 1 段（+0x18）が終わるまで第 2 段には
// 進まず、出力 +0x06 も更新されない — つまり第 1 段が遅延、第 2 段が立ち上がり。
// 出力は深さ（+0x00、符号付き）に立ち上がりカウンタを掛けた上位ワード。
//
// 実機との照合:
//   +0x18  9,194/9,194、3,007/3,007、5,105/5,105
//   +0x1a  6,979/6,979、14,167/14,167、13,655/13,655
//   +0x06  同上
struct ModulationRamp
{
    uint16_t delay = 0;      // +0x18
    uint16_t attack = 0;     // +0x1a
    uint16_t output = 0;     // +0x06
    bool     reached_lfo = false;   // 第 1 段が終わっていれば LFO 側へ進む
};

inline void StepModulationRamp (ModulationRamp& state, uint16_t delay_rate,
                                uint16_t attack_rate, int16_t depth, uint16_t tick)
{
    state.reached_lfo = true;

    if (state.delay != 0xffff)
    {
        const uint32_t product = (uint32_t) delay_rate * tick;
        const uint32_t low  = (product & 0xffff) + state.delay;
        const uint32_t high = ((product >> 16) & 0xffff) + (low >> 16);

        if ((high & 0xffff) != 0) state.delay = 0xffff;
        else
        {
            state.delay = (uint16_t) low;
            if (state.delay != 0xffff) return;    // まだ遅延中。ここで打ち切る
        }
    }

    if (state.attack == 0xffff)
        return;                                   // 完了後は別経路が値を配る

    const uint32_t product = (uint32_t) attack_rate * tick;
    const uint32_t low  = (product & 0xffff) + state.attack;
    const uint32_t high = ((product >> 16) & 0xffff) + (low >> 16);
    state.attack = ((high & 0xffff) != 0) ? 0xffff : (uint16_t) low;

    if (depth < 0)
    {
        const uint16_t magnitude = (uint16_t) (((uint32_t) (uint16_t) -depth * state.attack) >> 16);
        state.output = (uint16_t) (-(int16_t) magnitude);
    }
    else
    {
        state.output = (uint16_t) (((uint32_t) (uint16_t) depth * state.attack) >> 16);
    }
}

// Prepared off the control/audio thread; runtime uses integer lookup only.
struct LfoWaveformTables
{
    std::array<uint8_t,130> sine{};
    LfoWaveformTables()
    {
        for (unsigned i = 0; i < sine.size(); ++i) sine[i] = LfoSine(int(i));
    }
};

// 3bee..3d19. The caller supplies the FULL rate*ticks product, not the
// truncated LfoIncrement: random waveforms inspect its upper word too.
// PCM's channel-30 level is the random source; never substitute a constant.
struct LfoWaveformState
{
    uint16_t phase = 0, held = 0, smoothed = 0, output = 0;

    template<class Read,class Write>
    bool advance(uint8_t waveform,uint32_t increment,const LfoWaveformTables& tables,
        Read&& read,Write&& write)
    {
        if (waveform > 6) return false;
        if (waveform == 0)
            output = uint16_t(LfoSineStep(phase,uint16_t(increment),
                [&](int i) { return tables.sine[unsigned(i)]; }));
        else if (waveform < 4)
        {
            phase = uint16_t(phase+increment);
            output = uint16_t(waveform == 1 ? LfoSquare(phase)
                : waveform == 2 ? LfoSaw(phase) : LfoTrapezoid(phase));
        }
        else
        {
            const uint32_t doubled = increment<<1;
            const uint32_t low = (doubled&65535u)+phase;
            if (uint16_t((doubled>>16)+(low>>16)) != 0)
            {
                write(uint8_t(0x3e),uint8_t(30));
                (void)read(uint8_t(0x34));
                const auto high = read(uint8_t(0x3a));
                const auto lowByte = read(uint8_t(0x3b));
                held = uint16_t((uint16_t(high)<<8)|lowByte);
            }
            phase = uint16_t(low);
            if (waveform == 4) output = held;
            else
            {
                // 3cd0 falls through 3cd3: BOTH entries use 0x50.
                const int current = smoothed&32768 ? int(smoothed)-65536 : int(smoothed);
                const int target = held&32768 ? int(held)-65536 : int(held);
                smoothed = uint16_t(current < target ? std::min(current+80,target)
                    : std::max(current-80,target));
                output = smoothed;
            }
        }
        return true;
    }
};

// Native owner of one 34-byte modulation block, 3b2c..3d19. Delay gates
// the three depth ramps, NOT the oscillator. Rate data is prepared separately.
struct ModulationBlock
{
    std::array<uint16_t,3> depth{}, output{};
    uint16_t delay = 0, attack = 0, delayRate = 0, attackRate = 0;
    uint8_t rateIndex = 0, waveform = 0;
    int16_t rateModifier = 0;
    LfoWaveformState wave;

    template<class Read,class Write>
    bool advance(uint16_t ticks,const std::array<uint16_t,256>& rates,
        const LfoWaveformTables& tables,Read&& read,Write&& write)
    {
        if (waveform > 6) return false;
        const auto advanceCounter = [&](uint16_t value,uint16_t rate) {
            return uint16_t(std::min(65535u,uint32_t(value)+uint32_t(rate)*ticks));
        };
        delay = advanceCounter(delay,delayRate);
        if (delay == 65535)
        {
            if (attack == 65535) output = depth;
            else
            {
                attack = advanceCounter(attack,attackRate);
                for (unsigned i = 0; i < output.size(); ++i)
                {
                    const bool negative = (depth[i]&32768) != 0;
                    const uint16_t magnitude = negative ? uint16_t(0u-depth[i]) : depth[i];
                    const auto scaled = uint16_t((uint32_t(magnitude)*attack)>>16);
                    output[i] = negative ? uint16_t(0u-scaled) : scaled;
                }
            }
        }
        uint32_t rate = uint16_t(rates[rateIndex]+rateModifier);
        if (rate > 0x28f6) rate = rateModifier < 0 ? 0 : 0x28f6;
        return wave.advance(waveform,rate*ticks,tables,read,write);
    }
};

struct VoiceModulation
{
    ModulationBlock block; // second block, base voice-5e
    uint16_t firstStage = 0, partialIdentity = 0; // +00, +9e
    uint8_t field99 = 0, field9b = 0, sharing = 0; // -51 flag
    uint8_t fieldA2 = 0; // partial[8], captured only during gated initialization
};

// 3800..3890. Depths are sign/magnitude bytes, not signed two's complement.
// First-block pitch depth is deferred until live controller adjustment; return
// its raw parameter for FirstModulationInputs::pitchDepth. No runtime ROM reads.
inline uint8_t PrepareModulationDepths(const SC55Partial& partial,
    ModulationBlock& first,ModulationBlock& second,const ModulationDepthTables& tables) noexcept
{
    const auto signedValue = [](uint8_t parameter,uint16_t magnitude) {
        return parameter&128 ? uint16_t(0u-magnitude) : magnitude;
    };
    first.depth[0] = signedValue(partial.raw[0x48],uint16_t((partial.raw[0x48]&127)<<8));
    second.depth[0] = signedValue(partial.raw[0x49],uint16_t((partial.raw[0x49]&127)<<8));
    first.depth[1] = signedValue(partial.raw[0x2a],tables.secondEnvelope[partial.raw[0x2a]&127]);
    second.depth[1] = signedValue(partial.raw[0x2b],tables.secondEnvelope[partial.raw[0x2b]&127]);
    second.depth[2] = signedValue(partial.raw[0x0f],tables.pitch[partial.raw[0x0f]&127]);
    return partial.raw[0x0e];
}

// 39f6..3a71. The pitch-depth byte uses sign/magnitude; its controller
// moves the magnitude in opposite directions for negative/positive depths.
// Only rateIndex and depth[2] change. Phase, counters and other depths survive.
inline bool PrepareFirstModulationControls(ModulationBlock& block,uint8_t baseRate,
    uint8_t pitchDepth,uint8_t rateControl,uint8_t depthControl,
    const std::array<uint16_t,128>& pitchDepthTable) noexcept
{
    if (rateControl > 127 || depthControl > 127) return false;
    uint8_t rate = uint8_t(int(baseRate)+int(rateControl)-64);
    if (rate&128) rate = rateControl < 64 ? 0 : 127;
    const bool negative = (pitchDepth&128) != 0;
    const int adjustment = 2*(int(depthControl)-64);
    const auto index = unsigned(std::clamp(int(pitchDepth&127)+(negative ? -adjustment : adjustment),0,127));
    const auto depth = pitchDepthTable[index];
    block.rateIndex = rate;
    block.depth[2] = negative ? uint16_t(0u-depth) : depth;
    return true;
}

// 38e2..3950: local first-block preparation after source-sharing selection.
// Depths, held random value and rate fields are owned by adjacent preparation
// steps and survive. Timing table is the 256-word byte-indexed data at 7112.
inline bool PrepareFirstModulation(ModulationBlock& block,uint8_t mode,
    uint8_t delayParameter,uint8_t attackParameter,uint8_t delayControl,
    const std::array<uint16_t,256>& timing) noexcept
{
    if (delayControl > 127) return false;
    const auto selector = mode&15;
    block.waveform = uint8_t(selector <= 3 ? selector : selector >= 8 && selector <= 10 ? selector-4 : 0);
    block.wave.phase = uint16_t((mode&0xc0)<<8);
    block.wave.smoothed = block.wave.output = 0;
    block.delay = block.attack = 0; block.output = {};
    if (delayParameter&128) block.delayRate = 0;
    else
    {
        // The original saturates according to the sign of the BYTE result,
        // not a wide arithmetic clamp. Preserve the wrap at extreme settings.
        uint8_t adjusted;
        if (delayControl < 64)
        {
            adjusted = uint8_t(delayParameter-2*(64-delayControl));
            if (adjusted&128) adjusted = 0;
        }
        else
        {
            adjusted = uint8_t(delayParameter+2*(delayControl-64));
            if (adjusted&128) adjusted = 127;
        }
        block.delayRate = timing[adjusted];
    }
    block.attackRate = timing[attackParameter];
    return true;
}

enum class ModulationRoute { invalidInput, local, shared };

// 3d32..3e2d, block portion of first-modulation shared initialization.
// The caller owns source selection/link, raw base-rate and sharing metadata.
// Neither adjusted rateIndex nor destination depths are copied from source.
inline bool InitializeSharedFirstModulationBlock(ModulationBlock& block,
    const ModulationBlock& source,uint8_t pitchDepth,uint8_t depthControl,
    const std::array<uint16_t,128>& pitchDepthTable) noexcept
{
    if (depthControl > 127) return false;
    block.delayRate = source.delayRate; block.attackRate = source.attackRate;
    block.waveform = source.waveform; block.wave = source.wave;
    block.delay = source.delay; block.attack = source.attack;
    block.rateModifier = source.rateModifier;
    if (block.delay != 65535) { block.output = {}; return true; }
    const auto scale = [&](uint16_t depth) {
        const bool negative = (depth&0x8000) != 0;
        const uint16_t magnitude = negative ? uint16_t(0u-depth) : depth;
        const auto value = uint16_t((uint32_t(magnitude)*block.attack)>>16);
        return negative ? uint16_t(0u-value) : value;
    };
    for (unsigned i = 0; i < 2; ++i)
        block.output[i] = block.attack == 65535 ? block.depth[i] : scale(block.depth[i]);
    // Reuse the controller mapping, but this path leaves adjusted rate intact.
    auto mapped = block;
    PrepareFirstModulationControls(mapped,0,pitchDepth,64,depthControl,pitchDepthTable);
    block.depth[2] = mapped.depth[2];
    // Even at full attack pitch uses the high product word (not exact copy).
    block.output[2] = scale(block.depth[2]);
    return true;
}

struct FirstModulationSharing
{
    uint8_t source = 24; // Voice index; 24 means no source.
    uint8_t baseRate = 0, sharing = 0;
};

// Full 3d1a..3e2d transfer, after the caller has selected a live source.
// Source and destination may alias; validation precedes every mutation.
inline bool InitializeSharedFirstModulation(ModulationBlock& block,FirstModulationSharing& sharing,
    const ModulationBlock& source,const FirstModulationSharing& sourceSharing,
    uint8_t pitchDepth,uint8_t depthControl,const std::array<uint16_t,128>& depths) noexcept
{
    if (sourceSharing.source > 24 || depthControl > 127) return false;
    InitializeSharedFirstModulationBlock(block,source,pitchDepth,depthControl,depths);
    sharing = sourceSharing;
    return true;
}

struct FirstModulationInputs
{
    uint8_t mode = 0, baseRate = 0, delay = 0, attack = 0, pitchDepth = 0;
    uint8_t rateControl = 64, depthControl = 64, delayControl = 64;
};

// 38e2..3984, local (not shared) initialization. The initial random read
// occurs for EVERY waveform. The ensuing one-tick update may draw again.
// No global clock is changed, unlike the firmware's temporary ac5a override.
template<class Read,class Write>
bool InitializeFirstModulation(ModulationBlock& block,const FirstModulationInputs& input,
    const std::array<uint16_t,256>& timing,const std::array<uint16_t,128>& depths,
    const std::array<uint16_t,256>& rates,const LfoWaveformTables& tables,
    Read&& read,Write&& write)
{
    if (input.rateControl > 127 || input.depthControl > 127 || input.delayControl > 127) return false;
    auto next = block;
    PrepareFirstModulation(next,input.mode,input.delay,input.attack,input.delayControl,timing);
    write(uint8_t(0x3e),uint8_t(30)); (void)read(uint8_t(0x34));
    const auto high = read(uint8_t(0x3a)); const auto low = read(uint8_t(0x3b));
    next.wave.held = next.wave.smoothed = uint16_t((uint16_t(high)<<8)|low);
    PrepareFirstModulationControls(next,input.baseRate,input.pitchDepth,input.rateControl,input.depthControl,depths);
    next.advance(1,rates,tables,read,write);
    block = next;
    return true;
}

struct FirstModulationVoice
{
    ModulationBlock block;
    FirstModulationSharing sharing;
    uint16_t firstStage = 0, commonIdentity = 0;
    uint8_t commonBank = 0, field9b = 0;
};

// 38a5..38df: highest matching voice wins. The current slot is excluded;
// stage 12 is eligible, but later stages are not. 24 = none, 25 = bad input.
inline uint8_t SelectFirstModulationSource(unsigned channel,uint8_t mode,
    const std::array<FirstModulationVoice,24>& voices) noexcept
{
    if (channel >= voices.size()) return 25;
    if ((mode&16) == 0) return 24;
    const auto& voice = voices[channel];
    for (unsigned i = voices.size(); i-- > 0;)
    {
        const auto& candidate = voices[i];
        if (i != channel && candidate.firstStage <= 12 && candidate.field9b == voice.field9b
            && candidate.commonBank == voice.commonBank && candidate.commonIdentity == voice.commonIdentity)
            return uint8_t(i);
    }
    return 24;
}

// 3899..3984 plus shared tail. Caller supplies the current common-record
// inputs and owns stable voice lifetimes throughout this bounded operation.
template<class Read,class Write>
ModulationRoute InitializeFirstVoiceModulation(unsigned channel,
    std::array<FirstModulationVoice,24>& voices,const FirstModulationInputs& input,
    const std::array<uint16_t,256>& timing,const std::array<uint16_t,128>& depths,
    const std::array<uint16_t,256>& rates,const LfoWaveformTables& tables,Read&& read,Write&& write)
{
    if (channel >= voices.size() || input.rateControl > 127 || input.depthControl > 127 || input.delayControl > 127)
        return ModulationRoute::invalidInput;
    const auto source = SelectFirstModulationSource(channel,input.mode,voices);
    auto next = voices[channel];
    next.sharing.source = 24; next.sharing.sharing = 0;
    if (source < 24)
    {
        if (!InitializeSharedFirstModulation(next.block,next.sharing,voices[source].block,voices[source].sharing,
            input.pitchDepth,input.depthControl,depths)) return ModulationRoute::invalidInput;
    }
    else
    {
        InitializeFirstModulation(next.block,input,timing,depths,rates,tables,read,write);
        next.sharing.baseRate = input.baseRate;
    }
    voices[channel] = next;
    return source < 24 ? ModulationRoute::shared : ModulationRoute::local;
}

// 3d44 shared periodic tail, also called unconditionally for the second
// member of a periodic voice pair. Preserve destination configuration while
// copying oscillator progress, then derive outputs using its own depths.
inline bool UpdatePairedFirstModulation(FirstModulationVoice& destination,
    const FirstModulationVoice& source,uint8_t pitchDepth,uint8_t depthControl,
    const std::array<uint16_t,128>& depths) noexcept
{
    if (depthControl > 127) return false;
    auto copied = source.block;
    copied.delayRate = destination.block.delayRate;
    copied.attackRate = destination.block.attackRate;
    copied.waveform = destination.block.waveform;
    InitializeSharedFirstModulationBlock(destination.block,copied,pitchDepth,depthControl,depths);
    destination.sharing.sharing = source.sharing.sharing;
    return true;
}

// 3985..3a79, with a suspend/resume boundary for 39e1..39f5. The owning
// scheduler must keep the array and voice lifetimes stable across that boundary.
class FirstVoiceModulationUpdate
{
public:
    enum class Result { invalidInput, ready, shared, stageChanged, updated };
    Result begin(unsigned channel,std::array<FirstModulationVoice,24>& voices,
        uint8_t pitchDepth,uint8_t depthControl,const std::array<uint16_t,128>& depths) noexcept
    {
        if (pending_ || channel >= voices.size() || depthControl > 127) return Result::invalidInput;
        auto& voice = voices[channel];
        const bool wasSharing = voice.sharing.sharing != 0;
        if (wasSharing)
        {
            const auto source = voice.sharing.source;
            if (source >= voices.size()) return Result::invalidInput;
            const auto& other = voices[source];
            if (other.firstStage <= 12 && other.field9b == voice.field9b
                && other.commonBank == voice.commonBank && other.commonIdentity == voice.commonIdentity)
            {
                // Periodic sharing enters at 3d44, not at initialization's
                // 3d1a: preserve local timing, waveform selector and metadata.
                UpdatePairedFirstModulation(voice,other,pitchDepth,depthControl,depths);
                return Result::shared;
            }
            voice.sharing.source = 24; voice.sharing.sharing = 0;
            for (unsigned i = 0; i < voices.size(); ++i)
                if (i != channel && voices[i].sharing.source == source) voices[i].sharing.source = uint8_t(channel);
        }
        channel_ = channel; stage_ = voice.firstStage; checkStage_ = wasSharing; pending_ = true;
        return Result::ready;
    }

    template<class Read,class Write>
    Result resume(std::array<FirstModulationVoice,24>& voices,uint16_t ticks,
        uint8_t pitchDepth,uint8_t rateControl,uint8_t depthControl,
        const std::array<uint16_t,128>& depths,const std::array<uint16_t,256>& rates,
        const LfoWaveformTables& tables,Read&& read,Write&& write)
    {
        if (!pending_ || rateControl > 127 || depthControl > 127) return Result::invalidInput;
        pending_ = false;
        auto& voice = voices[channel_];
        if (checkStage_ && voice.firstStage != stage_) return Result::stageChanged;
        PrepareFirstModulationControls(voice.block,voice.sharing.baseRate,pitchDepth,rateControl,depthControl,depths);
        return voice.block.advance(ticks,rates,tables,read,write) ? Result::updated : Result::invalidInput;
    }
private:
    unsigned channel_ = 0;
    uint16_t stage_ = 0;
    bool pending_ = false, checkStage_ = false;
};

// 2b26..2bac: local second-block initialization after sharing selection.
// Unlike the first block, delay has no controller offset and rate is the
// unadjusted partial byte. All prepared depths and rateModifier survive.
template<class Read,class Write>
void InitializeSecondModulation(ModulationBlock& block,const SC55Partial& partial,
    const std::array<uint16_t,256>& timing,const std::array<uint16_t,256>& rates,
    const LfoWaveformTables& tables,Read&& read,Write&& write)
{
    PrepareFirstModulation(block,partial.raw[4],partial.raw[6],partial.raw[7],64,timing);
    block.rateIndex = partial.raw[5];
    write(uint8_t(0x3e),uint8_t(30)); (void)read(uint8_t(0x34));
    const auto high = read(uint8_t(0x3a)); const auto low = read(uint8_t(0x3b));
    block.wave.held = block.wave.smoothed = uint16_t((uint16_t(high)<<8)|low);
    block.advance(1,rates,tables,read,write);
}

// 2a87..2ac2: same descending policy as the first block, but compares the
// partial identity/bank rather than the common record. 24 = none, 25 = invalid.
inline uint8_t SelectSecondModulationSource(unsigned channel,uint8_t mode,
    const std::array<VoiceModulation,24>& voices) noexcept
{
    if (channel >= voices.size()) return 25;
    if ((mode&16) == 0) return 24;
    const auto& voice = voices[channel];
    for (unsigned i = voices.size(); i-- > 0;)
    {
        const auto& candidate = voices[i];
        if (i != channel && candidate.firstStage <= 12 && candidate.field9b == voice.field9b
            && candidate.field99 == voice.field99 && candidate.partialIdentity == voice.partialIdentity)
            return uint8_t(i);
    }
    return 24;
}

// 2a7b..2bac. Caller has applied the -3b bit-7 gate and copied partial[8]
// to voice+a2. Shared setup copies all block state EXCEPT destination depths;
// unlike the first block it copies outputs and adjusted rate without recompute.
template<class Read,class Write>
ModulationRoute InitializeSecondVoiceModulation(unsigned channel,
    std::array<VoiceModulation,24>& voices,std::array<uint8_t,24>& sources,
    const SC55Partial& partial,const std::array<uint16_t,256>& timing,
    const std::array<uint16_t,256>& rates,const LfoWaveformTables& tables,Read&& read,Write&& write)
{
    if (channel >= voices.size()) return ModulationRoute::invalidInput;
    const auto source = SelectSecondModulationSource(channel,partial.raw[4],voices);
    auto& voice = voices[channel];
    if (source < 24)
    {
        const auto depths = voice.block.depth;
        voice.block = voices[source].block;
        voice.block.depth = depths;
        voice.sharing = 255;
    }
    else
    {
        voice.sharing = 0;
        InitializeSecondModulation(voice.block,partial,timing,rates,tables,read,write);
    }
    sources[channel] = source;
    return source < 24 ? ModulationRoute::shared : ModulationRoute::local;
}

// 3a7a..3b11: indices replace firmware RAM pointers. 24 is the no-source
// sentinel. Does not emulate the interrupt window at 3b14..3b25; the outer
// owner must recheck voice lifetime before calling the local block updater.
inline ModulationRoute RouteVoiceModulation(unsigned channel,
    std::array<VoiceModulation,24>& voices,std::array<uint8_t,24>& sources) noexcept
{
    if (channel >= voices.size()) return ModulationRoute::invalidInput;
    auto& voice = voices[channel];
    if (voice.sharing == 0) return ModulationRoute::local;
    const auto source = sources[channel];
    if (source >= voices.size()) return ModulationRoute::invalidInput;
    const auto& other = voices[source];
    if (other.firstStage <= 12 && voice.field99 == other.field99
        && voice.field9b == other.field9b && voice.partialIdentity == other.partialIdentity)
    {
        voice.block.output = other.block.output;
        voice.block.rateModifier = other.block.rateModifier;
        voice.block.wave = other.block.wave;
        voice.block.delay = other.block.delay;
        voice.block.attack = other.block.attack;
        return ModulationRoute::shared;
    }
    sources[channel] = 24; voice.sharing = 0;
    for (unsigned i = 0; i < sources.size(); ++i)
        if (i != channel && sources[i] == source) sources[i] = uint8_t(channel);
    return ModulationRoute::local;
}

// One serialized control task. begin() routes/copies; resume() runs the local
// block at the caller's current control tick. The stage comparison applies
// only after detaching a shared source (3b11..3b25), not the direct local path.
// Caller owns the voice array for the task lifetime and must prevent slot reuse;
// firmware's stage comparison alone is not a generation/lifetime guarantee.
class VoiceModulationUpdate
{
public:
    enum class Result { invalidInput, ready, shared, stageChanged, updated };
    Result begin(unsigned channel,std::array<VoiceModulation,24>& voices,
        std::array<uint8_t,24>& sources) noexcept
    {
        if (pending_ || channel >= voices.size()) return Result::invalidInput;
        const bool wasSharing = voices[channel].sharing != 0;
        const auto route = RouteVoiceModulation(channel,voices,sources);
        if (route == ModulationRoute::invalidInput) return Result::invalidInput;
        if (route == ModulationRoute::shared) return Result::shared;
        channel_ = channel; stage_ = voices[channel].firstStage;
        checkStage_ = wasSharing; pending_ = true;
        return Result::ready;
    }

    template<class Read,class Write>
    Result resume(std::array<VoiceModulation,24>& voices,uint16_t ticks,
        const std::array<uint16_t,256>& rates,const LfoWaveformTables& tables,
        Read&& read,Write&& write)
    {
        if (!pending_) return Result::invalidInput;
        pending_ = false;
        auto& voice = voices[channel_];
        if (checkStage_ && voice.firstStage != stage_) return Result::stageChanged;
        return voice.block.advance(ticks,rates,tables,read,write) ? Result::updated : Result::invalidInput;
    }
private:
    unsigned channel_ = 0;
    uint16_t stage_ = 0;
    bool pending_ = false, checkStage_ = false;
};

} // namespace sc55
