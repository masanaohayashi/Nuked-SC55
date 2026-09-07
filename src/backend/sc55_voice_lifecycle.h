#pragma once
#include "sc55_voice_allocator.h"
#include "sc55_envelope_pcm.h"
#include "sc55_voice_setup.h"
#include "sc55_pitch.h"
#include "sc55_lfo.h"
#include "sc55_level.h"
#include "sc55_second_envelope_tables.h"
#include <algorithm>
#include <span>

namespace sc55
{
// 47fb..4856. Not identical to pitch modulation: the two-negative branch
// uses only the first depth (4813..4817 has no addition of the second).
inline uint16_t ApplySecondEnvelopeModulation(uint16_t level,uint16_t first,
    uint16_t second,uint16_t wave) noexcept
{
    bool negative;
    uint16_t magnitude;
    if ((first&0x8000) == (second&0x8000))
    {
        negative = (first&0x8000) != 0;
        magnitude = negative ? uint16_t(0u-first) : uint16_t(first+second);
        if (magnitude > 6144) magnitude = 6144;
    }
    else
    {
        magnitude = uint16_t(first+second); negative = (magnitude&0x8000) != 0;
        if (negative) magnitude = uint16_t(0u-magnitude);
    }
    if (wave&0x8000) { wave = uint16_t(0u-wave); negative = !negative; }
    const uint16_t amount = uint16_t((uint32_t(magnitude)*uint16_t(wave*2u)+32768u)>>16);
    if (negative) return level < amount ? uint16_t(0) : uint16_t(level-amount);
    const uint16_t result = uint16_t(level+amount);
    return (~(level^amount)&(level^result)&0x8000) != 0 ? uint16_t(32767) : result;
}

struct SecondEnvelopeOutputInputs
{
    uint8_t base = 0, control = 64;
    bool suppressPositiveControl = false;
    uint16_t offset = 0;
    struct Source { uint16_t first = 0, second = 0, waveform = 0; };
    std::array<Source,2> sources{};
};

// Modulation blocks at voice-80/-5e feed distinct depth outputs:
// +08 -> second envelope (46c3..46da), +0a -> pitch (510a..5121).
// +20 is their shared waveform. Additional controller depths (source.second),
// offsets, base/control values and tuning remain owned by their input producers.
inline void ApplyVoiceModulationOutputs(const ModulationBlock& first,
    const ModulationBlock& second,SecondEnvelopeOutputInputs& envelope,
    PitchModulationInputs& pitch) noexcept
{
    const ModulationBlock* blocks[2]{&first,&second};
    for (unsigned i = 0; i < 2; ++i)
    {
        envelope.sources[i].first = blocks[i]->output[1];
        pitch.sources[i].first = blocks[i]->output[2];
        envelope.sources[i].waveform = blocks[i]->wave.output;
        pitch.sources[i].waveform = blocks[i]->wave.output;
    }
}

// 30ff..3115 adds the amplitude consumer: output[0] is the signed depth
// and wave.output is the waveform (historically called mod*_depth here).
// Preserve live expression/volume/bias and additional controller depths.
inline void ApplyVoiceModulationOutputs(const ModulationBlock& first,
    const ModulationBlock& second,LevelInputs& level,SecondEnvelopeOutputInputs& envelope,
    PitchModulationInputs& pitch) noexcept
{
    ApplyVoiceModulationOutputs(first,second,envelope,pitch);
    level.mod1_a = int16_t(first.output[0]); level.mod2_a = int16_t(second.output[0]);
    level.mod1_depth = int16_t(first.wave.output); level.mod2_depth = int16_t(second.wave.output);
}

struct SecondEnvelopeTargetInputs { uint16_t start = 0, depth = 0, scale = 0; };

// 3e98..3f16. keyValue is the already-selected key-table word (center 4000).
// The velocity multiply deliberately retains its LOW word, unlike the start
// multiply which takes bits 8..23. Table selectors are raw parameter bytes.
inline SecondEnvelopeTargetInputs PrepareSecondEnvelopeTargetInputs(
    const SC55Partial& partial,uint16_t keyValue,uint8_t amplitude,
    const SecondEnvelopeTargetTables& tables) noexcept
{
    const auto sensitivity = partial.raw[0x29];
    const unsigned index = sensitivity < 64 ? 64u-sensitivity : sensitivity-64u;
    const auto keyMagnitude = keyValue < 16384 ? uint16_t(16384-keyValue) : uint16_t(keyValue-16384);
    const auto value = uint16_t((uint32_t(keyMagnitude)*tables.startSensitivity[index])>>8);
    const bool negative = (sensitivity < 64) != (keyValue < 16384);
    const uint16_t start = negative ? uint16_t(0u-value) : value;
    const auto velocitySensitivity = partial.raw[0x3d];
    uint16_t depth = 32767;
    if (velocitySensitivity != 64)
    {
        const unsigned at = velocitySensitivity < 64 ? 64u-velocitySensitivity : velocitySensitivity-64u;
        depth = uint16_t(32767u-uint16_t(uint8_t(127-amplitude)*uint32_t(tables.velocitySensitivity[at])));
        if (velocitySensitivity < 64) depth = uint16_t(0u-depth);
    }
    return {start,depth,tables.scale[partial.raw[0x2c]]};
}

// 410d..418c: signed target relative to the prepared start level. Products
// truncate after each scaling step; saturation is branch-specific, not a
// final generic signed clamp. The curve is shared with pitch preparation.
inline uint16_t PrepareSecondEnvelopeTarget(uint16_t start,uint16_t depth,
    uint16_t scale,uint8_t parameter,const std::array<uint8_t,256>& curve) noexcept
{
    const bool negative = (parameter < 64) != ((depth&0x8000) != 0);
    const auto magnitude = (depth&0x8000) ? uint16_t(0u-depth) : depth;
    const unsigned index = parameter < 64 ? 64u-parameter : parameter-64u;
    const uint16_t scaled = uint16_t((uint32_t(magnitude)*scale)>>15);
    const uint16_t amount = uint16_t((uint32_t(scaled)*curve[index])>>7);
    if (negative)
    {
        if (!(start&0x8000)) return uint16_t(start-amount);
        const uint16_t sum = uint16_t(uint16_t(0u-start)+amount);
        return (sum&0x8000) ? uint16_t(0x8001) : uint16_t(0u-sum);
    }
    if (start&0x8000) return uint16_t(amount-uint16_t(0u-start));
    const uint16_t sum = uint16_t(start+amount);
    return (sum&0x8000) ? uint16_t(0x7fff) : sum;
}

// 4662..46e0, through both modulation calls. Output is distinct from the
// signed internal envelope level. Musical base/control inputs are 7-bit.
inline std::optional<uint16_t> PrepareSecondEnvelopeOutput(uint16_t level,
    const SecondEnvelopeOutputInputs& input) noexcept
{
    if (input.base > 127 || input.control > 127) return std::nullopt;
    int base = input.base;
    if (input.control < 64) base = std::max(0,base+int(input.control)-64);
    else if (!input.suppressPositiveControl) base = std::min(127,base+std::min(16,int(input.control)-64));
    const int signedLevel = level&0x8000 ? int(level)-65536 : int(level);
    uint16_t value = uint16_t(std::clamp(signedLevel+base*256,0,32767));
    if (input.offset&0x8000)
    {
        const uint16_t magnitude = uint16_t(0u-input.offset);
        value = value < magnitude ? uint16_t(0) : uint16_t(value-magnitude);
    }
    else value = uint16_t(std::min(32767u,unsigned(value)+unsigned(input.offset)));
    for (const auto& source : input.sources)
        value = ApplySecondEnvelopeModulation(value,source.first,source.second,source.waveform);
    return value;
}

// 46e0..473c: approach the requested byte by one, then apply a ceiling
// derived from the previous PCM level. An unchanged request skips the ceiling.
inline std::optional<uint8_t> AdvanceSecondEnvelopeControl(uint8_t current,
    uint8_t base,uint8_t control,uint8_t limit,uint16_t previousLevel,
    const SecondEnvelopePcmTables& tables) noexcept
{
    if (base > 127 || control > 127 || current > 127 || limit > 127) return std::nullopt;
    const int adjusted = int(base)+2*(64-int(control));
    const auto desired = uint8_t(std::min(int(limit),std::max(0,adjusted)));
    if (current == desired) return current;
    const auto next = uint8_t(current < desired ? current+1 : current-1);
    const unsigned index = std::min(255u,(unsigned(previousLevel)+255u)>>8);
    return std::min(next,tables.smoothingCeiling[index]);
}

// 473c..478e: interpolate before doubling with word wrap; this also enforces
// a minimum controller byte of eight, even when smoothing did not change it.
inline std::optional<uint16_t> ConvertSecondEnvelopePcmLevel(uint16_t level,uint8_t& control,
    const SecondEnvelopePcmTables& tables) noexcept
{
    if (level > 32767 || control > 127) return std::nullopt;
    const unsigned index = level>>8, fraction = level&255;
    const uint16_t base = tables.levelCurve[index];
    const uint16_t difference = uint16_t(tables.levelCurve[index+1]-base);
    const uint16_t interpolated = uint16_t(base+((uint32_t(difference)*fraction)>>8));
    control = std::max(uint8_t(8),control);
    return std::min({uint16_t(interpolated*2u),uint16_t(tables.outputCeiling[control]<<8),uint16_t(0xe600)});
}

// 478e..47fa. The cached level is NOT the rounded-up command target.
// stepTime comes from the segment dispatcher (0..8), not the full duration.
inline std::optional<uint16_t> EncodeSecondEnvelopePcmCommand(uint16_t previous,
    uint16_t level,uint8_t control,uint16_t stepTime,
    const SecondEnvelopePcmTables& tables) noexcept
{
    if (level > 0xe600 || control < 8 || control > 127 || stepTime > 8)
        return std::nullopt;
    if (previous == level) return uint16_t(0xff00);
    const uint16_t magnitude = previous > level ? uint16_t(previous-level) : uint16_t(level-previous);
    uint16_t target = level;
    if (level > previous)
    {
        target &= 0xff00;
        if (target == (previous&0xff00))
            target = std::min(uint16_t(target+0x100),uint16_t(tables.outputCeiling[control]<<8));
    }
    static constexpr uint8_t limits[9] {10,10,9,9,8,8,8,7,7};
    return EncodeCutoff(target,magnitude,limits[stepTime],stepTime == 0);
}

struct SecondEnvelopePcmState
{
    uint8_t control = 8;
    uint16_t output = 0, level = 0, command = 0xff00;

    // 4662..47fa, with transactional validation of caller-owned inputs.
    bool advance(uint16_t internalLevel,const SecondEnvelopeOutputInputs& input,
        uint8_t base,uint8_t controller,uint8_t limit,uint16_t stepTime,
        const SecondEnvelopePcmTables& tables) noexcept
    {
        const auto nextOutput = PrepareSecondEnvelopeOutput(internalLevel,input);
        auto nextControl = AdvanceSecondEnvelopeControl(control,base,controller,limit,level,tables);
        if (!nextOutput || !nextControl) return false;
        const auto nextLevel = ConvertSecondEnvelopePcmLevel(*nextOutput,*nextControl,tables);
        if (!nextLevel) return false;
        const auto nextCommand = EncodeSecondEnvelopePcmCommand(level,*nextLevel,*nextControl,stepTime,tables);
        if (!nextCommand) return false;
        output = *nextOutput; control = *nextControl; level = *nextLevel; command = *nextCommand;
        return true;
    }
};

// Second envelope state; duration preparation/dispatch and post-processing
// remain separate from the segment interpolation implemented here.
struct SecondEnvelopeReleaseState
{
    uint16_t stage = 0;
    EnvelopeProgress progress{};
    uint8_t parameter = 0, releaseParameter = 0;
    uint16_t level = 0, start = 0, target = 0, releaseTarget = 0;
    uint16_t stepTime = 0; // voice-30, consumed by downstream processing
    std::array<uint8_t,3> nextParameters{};
    std::array<uint16_t,3> nextTargets{};
    enum class Result { invalidInput, idle, updated };

    // 3f13..418c and 43d1..4403: install all five target/time parameters
    // after start/depth/scale preparation. Do not reset a running segment;
    // initialization is a separate firmware entry.
    void installTargets(const SC55Partial& partial,uint16_t preparedStart,
        uint16_t depth,uint16_t scale,const std::array<uint8_t,256>& curve) noexcept
    {
        start = preparedStart;
        target = PrepareSecondEnvelopeTarget(start,depth,scale,partial.raw[0x2d],curve);
        for (unsigned i = 0; i < 3; ++i)
        {
            nextTargets[i] = PrepareSecondEnvelopeTarget(start,depth,scale,partial.raw[0x2e + i],curve);
            nextParameters[i] = partial.raw[0x33+i]&127;
        }
        releaseTarget = PrepareSecondEnvelopeTarget(start,depth,scale,partial.raw[0x31],curve);
        parameter = partial.raw[0x32]&127;
        releaseParameter = partial.raw[0x36]&127;
    }

    // 3e79..418c plus time-parameter installation. Raw key-table selector
    // uses only its low nibble; key is the firmware's prepared key byte.
    void prepareTargets(const SC55Partial& partial,uint8_t key,uint8_t amplitude,
        const SecondEnvelopeKeyTables& keys,const SecondEnvelopeTargetTables& tables,
        const std::array<uint8_t,256>& curve) noexcept
    {
        const auto input = PrepareSecondEnvelopeTargetInputs(partial,keys[partial.raw[0x28]&15][key],amplitude,tables);
        installTargets(partial,input.start,input.depth,input.scale,curve);
    }

    // 4443..4662, stopping before post-processing. voice65 bypasses all
    // mutation. Stage0 also clears the cached PCM36 level, not its command.
    Result advance(bool bypass,uint16_t ticks,const SecondEnvelopeTiming& timing,
        const EnvelopeTimes& times,uint16_t& pcmLevel36) noexcept
    {
        if (bypass) return Result::idle;
        if (stage > 22 || (stage&1)) return Result::invalidInput;
        auto next = *this;
        if (next.progress.position == 65535)
        {
            next.progress.position = 0;
            next.stage = stage < 12 ? uint16_t(stage+2) : uint16_t(22);
            if (next.stage >= 4 && next.stage <= 8)
            {
                const unsigned index = (next.stage-4)/2;
                next.start = next.target;
                next.target = next.nextTargets[index];
                next.parameter = next.nextParameters[index];
            }
        }
        Result result = Result::updated;
        if (next.stage == 0)
        { next.level = next.start; next.stepTime = 0; }
        else if (next.stage == 10 || next.stage == 22)
        { next.level = next.target; next.stepTime = 8; next.progress.deferredTicks = 0; }
        else if (next.stage >= 14) result = Result::idle;
        else
        {
            const auto interval = next.stage <= 4 ? SecondEnvelopeInterval::attack
                : next.stage <= 8 ? SecondEnvelopeInterval::decay : SecondEnvelopeInterval::release;
            if (!next.advanceInterval(interval,timing,times,ticks)) return Result::invalidInput;
        }
        *this = next;
        if (stage == 0) pcmLevel36 = 0;
        return result;
    }

    bool advanceInterval(SecondEnvelopeInterval interval,const SecondEnvelopeTiming& timing,
        const EnvelopeTimes& times,uint16_t ticks) noexcept
    {
        const auto duration = timing.duration(parameter,interval,times);
        if (!duration) return false;
        advanceSegment(*duration,ticks);
        return true;
    }

    // 45b6..4662. A short duration snaps without consuming deferred ticks.
    // Overflow snaps exactly; merely reaching ffff still interpolates.
    void advanceSegment(uint16_t duration,uint16_t ticks) noexcept
    {
        if (duration <= 8)
        {
            progress.position = 65535;
            stepTime = duration;
            level = target;
            return;
        }
        stepTime = 8;
        const uint16_t increment = uint16_t(524288u/duration);
        const uint16_t elapsed = uint16_t(ticks+progress.deferredTicks);
        const uint32_t position = uint32_t(elapsed)*increment+progress.position;
        progress.deferredTicks = 0;
        if (position > 65535)
        {
            progress.position = 65535;
            progress.deferredTicks = uint16_t((position-65535)/increment);
            level = target;
            return;
        }
        progress.position = uint16_t(position);
        const auto signedWord = [](uint16_t value) { return value&0x8000 ? int32_t(value)-65536 : int32_t(value); };
        const int32_t difference = signedWord(target)-signedWord(start);
        uint32_t distance = uint32_t(difference < 0 ? -difference : difference);
        if (((start^target)&0x8000) != 0 && distance > 32767) distance = 32767;
        const uint32_t amount = (distance*position)>>16;
        level = uint16_t(start+(difference < 0 ? 0u-amount : amount));
    }
};

struct SecondEnvelopeInitialControl { uint8_t limit = 0, control = 0; };

// 418c..4253. Signed maximum includes the start and all five targets. This
// path rounds UP before each lookup, without the normal output interpolation,
// modulation or one-step controller smoothing.
inline std::optional<SecondEnvelopeInitialControl> PrepareSecondEnvelopeInitialControl(
    const SecondEnvelopeReleaseState& segment,uint8_t levelBase,uint8_t levelControl,
    bool suppressPositiveControl,uint8_t controlBase,uint8_t controller,
    const SecondEnvelopePcmTables& tables) noexcept
{
    if (controlBase > 127 || controller > 127) return std::nullopt;
    const auto signedWord = [](uint16_t value) { return value&0x8000 ? int(value)-65536 : int(value); };
    int maximum = signedWord(segment.start);
    for (auto value : {segment.target,segment.nextTargets[0],segment.nextTargets[1],segment.nextTargets[2],segment.releaseTarget})
        maximum = std::max(maximum,signedWord(value));
    SecondEnvelopeOutputInputs input;
    input.base = levelBase; input.control = levelControl; input.suppressPositiveControl = suppressPositiveControl;
    const auto adjusted = PrepareSecondEnvelopeOutput(uint16_t(maximum),input);
    if (!adjusted) return std::nullopt;
    const unsigned curveIndex = std::min(32767u,unsigned(*adjusted)+255u)>>8;
    const uint16_t doubled = uint16_t(tables.levelCurve[curveIndex]*2u);
    const unsigned ceilingIndex = std::min(255u,(unsigned(doubled)+255u)>>8);
    const auto limit = tables.smoothingCeiling[ceilingIndex];
    const auto control = uint8_t(std::clamp(int(controlBase)+2*(64-int(controller)),0,int(limit)));
    return SecondEnvelopeInitialControl{limit,control};
}

// Complete periodic second-envelope calculation (4443..47fa). Release owns
// the segment state; the PCM handoff owns the output state. Both are committed
// together, and bypass/inactive stages must not run post-processing.
inline SecondEnvelopeReleaseState::Result AdvanceSecondEnvelope(
    SecondEnvelopeReleaseState& segment,SecondEnvelopePcmState& pcm,
    bool bypass,uint16_t ticks,const SecondEnvelopeTiming& timing,const EnvelopeTimes& times,
    const SecondEnvelopeOutputInputs& input,uint8_t base,uint8_t controller,uint8_t limit,
    const SecondEnvelopePcmTables& tables) noexcept
{
    using Result = SecondEnvelopeReleaseState::Result;
    auto nextSegment = segment;
    auto nextPcm = pcm;
    const auto result = nextSegment.advance(bypass,ticks,timing,times,nextPcm.level);
    if (result == Result::invalidInput) return result;
    if (result == Result::updated && !nextPcm.advance(nextSegment.level,input,base,controller,limit,nextSegment.stepTime,tables))
        return Result::invalidInput;
    segment = nextSegment; pcm = nextPcm;
    return result;
}

struct SecondEnvelopeActivation
{
    uint16_t level = 0, command = 0;
};

struct SecondEnvelopeMode { bool bypass = false; uint8_t pcmMode = 1; };

// 3e3b..3e69 / 4435. Modes 0/1 continue normal preparation. All other
// bytes bypass it, clearing PCM/activation levels and commands but preserving
// the internal envelope and its already-computed output.
inline SecondEnvelopeMode ConfigureSecondEnvelopeMode(uint8_t mode,
    SecondEnvelopePcmState& pcm,SecondEnvelopeActivation& activation) noexcept
{
    if (mode < 2) return {false,uint8_t(mode*2+1)};
    pcm.level = pcm.command = 0;
    pcm.control = 64;
    activation = {};
    return {true,3};
}

// 4403..4435, after target/timing preparation. First evaluate the start value
// for activation, then reset progress and calculate one attack tick. Neither
// the raw stage nor the caller's elapsed-tick clock is changed.
inline std::optional<SecondEnvelopeActivation> InitializeSecondEnvelope(
    SecondEnvelopeReleaseState& segment,SecondEnvelopePcmState& pcm,
    const SecondEnvelopeTiming& timing,const EnvelopeTimes& times,
    const SecondEnvelopeOutputInputs& input,uint8_t base,uint8_t controller,uint8_t limit,
    const SecondEnvelopePcmTables& tables) noexcept
{
    auto nextSegment = segment;
    auto nextPcm = pcm;
    nextPcm.level = 0;
    nextSegment.level = nextSegment.start;
    nextSegment.stepTime = 0;
    if (!nextPcm.advance(nextSegment.level,input,base,controller,limit,0,tables)) return std::nullopt;
    const SecondEnvelopeActivation activation{nextPcm.level,uint16_t((nextPcm.command&0xff00)|0xba)};
    nextSegment.progress = {};
    if (!nextSegment.advanceInterval(SecondEnvelopeInterval::attack,timing,times,1)
        || !nextPcm.advance(nextSegment.level,input,base,controller,limit,nextSegment.stepTime,tables))
        return std::nullopt;
    segment = nextSegment; pcm = nextPcm;
    return activation;
}

struct SecondEnvelopeSetup
{
    SecondEnvelopeOutputInputs input;
    SecondEnvelopeTiming timing;
    SecondEnvelopeMode mode;
    SecondEnvelopeActivation activation;
    uint8_t controlBase = 0, controller = 64, limit = 0;

    // 3e3b..4435. Caller supplies existing controllers/modulation inputs and
    // the prepared key/amplitude; all resulting state commits atomically here.
    bool prepare(const SC55Partial& partial,uint8_t key,uint8_t amplitude,
        SecondEnvelopeReleaseState& segment,SecondEnvelopePcmState& pcm,
        const SecondEnvelopeKeyTables& keys,const SecondEnvelopeTargetTables& targets,
        const std::array<uint8_t,256>& curve,const SecondEnvelopeTimingKeyCurves& timingCurves,
        const std::array<uint16_t,256>& multipliers,const EnvelopeTimes& times,
        const SecondEnvelopePcmTables& pcmTables) noexcept
    {
        auto next = *this; auto nextSegment = segment; auto nextPcm = pcm;
        next.mode = ConfigureSecondEnvelopeMode(partial.raw[0x27],nextPcm,next.activation);
        if (!next.mode.bypass)
        {
            next.input.base = partial.raw[0x25]; next.controlBase = partial.raw[0x26];
            nextSegment.prepareTargets(partial,key,amplitude,keys,targets,curve);
            const auto initial = PrepareSecondEnvelopeInitialControl(nextSegment,next.input.base,next.input.control,
                next.input.suppressPositiveControl,next.controlBase,next.controller,pcmTables);
            auto scales = PrepareSecondEnvelopeTiming(partial,key,amplitude,timingCurves,multipliers);
            if (!initial || !scales) return false;
            next.limit = initial->limit; nextPcm.control = initial->control;
            scales->attack = next.timing.attack; scales->decay = next.timing.decay; scales->release = next.timing.release;
            scales->attackControlEnabled = next.timing.attackControlEnabled;
            next.timing = *scales;
            const auto activation = InitializeSecondEnvelope(nextSegment,nextPcm,next.timing,times,next.input,
                next.controlBase,next.controller,next.limit,pcmTables);
            if (!activation) return false;
            next.activation = *activation;
        }
        *this = next; segment = nextSegment; pcm = nextPcm;
        return true;
    }
};

struct VoiceReleaseAuxiliary
{
    uint8_t pending = 0, activity = 0;
    SecondEnvelopeReleaseState second;
    // Raw words at voice-70/-4e; zero becomes ffff only on active release.
    std::array<uint16_t,2> modulationReleaseWords{};
};

struct VoicePcmLevelState
{
    uint16_t level32 = 0, command16 = 0;
    uint16_t level36 = 0, command1a = 0;
};

// 3196..3212: synchronize PCM32, amplitude34, then PCM36. These levels
// are distinct from the second envelope's internal level at voice+20.
// Delayed/finished voices preserve levels AND activity. Invalid channel
// returns false before I/O or mutation. Caller then consumes release requests.
template<class Read,class Write>
bool SynchronizeVoicePcm(uint8_t channel,EnvelopeRunner& amplitude,
    VoiceReleaseAuxiliary& release,VoicePcmLevelState& levels,Read&& read,Write&& write)
{
    if (channel >= 24) return false;
    const auto stage = amplitude.state().segment.stage;
    if (stage == EnvelopeStage::delay || stage >= EnvelopeStage::finished) return true;
    write(uint8_t(0x3e),channel);
    levels.level32 = SynchronizePcmRamp(levels.command16,levels.level32,0x16,0x32,read,write);
    SynchronizeEnvelopePcm(amplitude,read,write);
    const auto activity = uint8_t(amplitude.state().level>>8);
    release.activity = activity == 255 ? 254 : activity;
    levels.level36 = SynchronizePcmRamp(levels.command1a,levels.level36,0x1a,0x36,read,write);
    return true;
}

// Native second-envelope handoff: read the hardware level directly back into
// its owner. The legacy mapping is only a local adapter, never retained state.
template<class Read,class Write>
bool SynchronizeVoicePcm(uint8_t channel,EnvelopeRunner& amplitude,
    VoiceReleaseAuxiliary& release,uint16_t& level32,uint16_t command16,
    SecondEnvelopePcmState& second,Read&& read,Write&& write)
{
    VoicePcmLevelState mapped{level32,command16,second.level,second.command};
    if (!SynchronizeVoicePcm(channel,amplitude,release,mapped,read,write)) return false;
    level32 = mapped.level32; second.level = mapped.level36;
    return true;
}

// 3212..32f0/3363, AFTER PCM level synchronization. All owners belong to
// one voice and must be updated on the same control thread. nullopt means
// no request; cancel tells the scheduler to skip the normal voice updates.
inline std::optional<VoiceReleaseAction> ConsumeVoiceRelease(VoiceReleaseAuxiliary& aux,
    EnvelopeRunner& amplitude,PitchEnvelopeRunner& pitch) noexcept
{
    if (aux.pending == 0) return std::nullopt;
    aux.pending = 0;
    const auto stage = amplitude.state().segment.stage;
    const uint16_t firstStage = stage == EnvelopeStage::finished ? 22 : uint16_t(unsigned(stage)*2);
    const auto action = pitch.requestRelease(firstStage,amplitude.setup().delayIncrement);
    if (action == VoiceReleaseAction::unchanged) return action;
    amplitude.release(amplitude.state().level);
    aux.second.stage = action == VoiceReleaseAction::start ? 2 : action == VoiceReleaseAction::cancel ? 22 : 12;
    if (action == VoiceReleaseAction::cancel) aux.activity = 0;
    if (action == VoiceReleaseAction::release)
    {
        aux.second.progress = {};
        aux.second.parameter = aux.second.releaseParameter;
        aux.second.start = aux.second.level;
        aux.second.target = aux.second.releaseTarget;
        for (auto& word : aux.modulationReleaseWords) if (word == 0) word = 65535;
    }
    return action;
}

enum class VoiceUpdateEntry { invalidChannel, stopped, update };

// 3196..32f0/3363. The stopped gate precedes release consumption: a pending
// request on a finished voice must survive for the outer lifecycle owner.
// Active release must start from the PCM readback, not the previous software
// amplitude. Caller serializes all device access and handles stopped voices
// separately; this does not run the modulation/envelope dispatch that follows.
template<class Read,class Write>
VoiceUpdateEntry BeginVoiceUpdate(uint8_t channel,EnvelopeRunner& amplitude,
    VoiceReleaseAuxiliary& release,PitchEnvelopeRunner& pitch,
    uint16_t& level32,uint16_t command16,SecondEnvelopePcmState& second,
    Read&& read,Write&& write)
{
    if (channel >= 24) return VoiceUpdateEntry::invalidChannel;
    if (amplitude.state().segment.stage >= EnvelopeStage::finished)
        return VoiceUpdateEntry::stopped;
    SynchronizeVoicePcm(channel,amplitude,release,level32,command16,second,read,write);
    const auto action = ConsumeVoiceRelease(release,amplitude,pitch);
    return action == VoiceReleaseAction::cancel ? VoiceUpdateEntry::stopped : VoiceUpdateEntry::update;
}

template<class Read,class Write>
VoiceUpdateEntry BeginVoiceUpdate(uint8_t channel,EnvelopeRunner& amplitude,
    VoiceReleaseAuxiliary& release,PitchEnvelopeRunner& pitch,TvaState& tva,
    SecondEnvelopePcmState& second,Read&& read,Write&& write)
{
    return BeginVoiceUpdate(channel,amplitude,release,pitch,tva.level,tva.command,second,read,write);
}

// Stop-related fields owned by each physical voice. These are intentionally
// distinct from allocator status (a slot can be reusable while PCM ramps).
struct VoiceStopState
{
    std::array<uint16_t,3> stages{}; // voice +00/+02/+04
    uint16_t cached16 = 0, cached18 = 0; // voice +1a/+1e
    uint8_t fieldCB30 = 0, fieldCAF4 = 0;
    uint16_t savedStage = 0, progress = 0, delayAccumulator = 0, pcm10 = 0;
    uint8_t flagMinus3B = 0; // branch input at voice-3b, meaning not inferred
    uint8_t fieldC8B3 = 0;
};

// Complete 53e6..542f state/device operation. Does not change CAF4 or allocator
// ownership: restart and reclamation have different post-stop bookkeeping.
template<class Read,class Write>
bool StopPreparedVoice(unsigned channel,VoiceStopState& state,Read&& read,Write&& write)
{
    if (channel >= 24) return false;
    state.fieldCB30 = 0;
    const auto plan = *StopVoicePcm(uint8_t(channel),read,write);
    if (plan.pcmAddress == 0x16) state.cached16 = 0x00b6;
    else state.cached18 = 0x00b6;
    state.stages.fill(plan.stageCode);
    return true;
}

// Key-on writes cached16 at 57c0. Periodic writes use TvaState directly;
// this transfer is only for the prepared activation batch.
inline void PrepareVoiceTva(VoiceStopState& voice,const TvaState& tva) noexcept
{
    voice.cached16 = tva.command;
}

enum class SecondModulationPreparation { invalidInput, unchanged, local, shared };

// 2a66..2bac: use the SAME -3b flag as voice activation, without consuming it.
// A skipped call must preserve oscillator phase, links and the cached partial[8].
template<class Read,class Write>
SecondModulationPreparation PrepareVoiceSecondModulation(unsigned channel,const VoiceStopState& state,
    std::array<VoiceModulation,24>& voices,std::array<uint8_t,24>& sources,
    const SC55Partial& partial,const std::array<uint16_t,256>& timing,
    const std::array<uint16_t,256>& rates,const LfoWaveformTables& tables,Read&& read,Write&& write)
{
    if (channel >= voices.size()) return SecondModulationPreparation::invalidInput;
    if ((state.flagMinus3B&128) == 0) return SecondModulationPreparation::unchanged;
    voices[channel].fieldA2 = partial.raw[8];
    const auto route = InitializeSecondVoiceModulation(channel,voices,sources,partial,timing,rates,tables,read,write);
    return route == ModulationRoute::shared ? SecondModulationPreparation::shared : SecondModulationPreparation::local;
}

// Pitch preparation shares the commit's cached +48 word and +04 stage.
// Other envelopes and readiness/allocator flags remain independently owned.
inline void PrepareVoicePitch(VoiceStopState& voice,const VoicePitchRunner& pitch) noexcept
{
    voice.stages[2] = pitch.envelope.stage;
    voice.pcm10 = pitch.pcmWord;
}

// Activation may set all three stages to delay/attack. It does NOT clear
// pitch progress or the cached pitch word when it writes zero to PCM10.
// Reflect the commit's stage without treating that temporary write as pitch.
inline std::optional<VoicePitchRunner> ContinueVoicePitch(const VoiceStopState& voice,
    const VoicePitchRunner& prepared) noexcept
{
    if (voice.stages[2] > 22 || (voice.stages[2]&1)) return std::nullopt;
    auto result = prepared;
    result.envelope.stage = voice.stages[2];
    return result;
}

// Transfer the first envelope's prepared fields to the voice-start owner.
// Other envelopes, savedStage, pitch and allocator flags are not overwritten.
inline void PrepareVoiceAmplitude(VoiceStopState& voice,const EnvelopeRunner& runner) noexcept
{
    const auto& state = runner.state();
    voice.stages[0] = state.segment.stage == EnvelopeStage::finished ? 22
        : uint16_t(unsigned(state.segment.stage)*2);
    voice.progress = state.segment.progress.position;
    voice.delayAccumulator = state.delayAccumulator;
    voice.cached18 = state.pcmWord;
}

// Continue from the actual commit result, rather than independently activating
// a second copy of the envelope. Only commit-owned fields change; level,
// deferred ticks, parameter and targets survive from the prepared runner.
// Stop/special dispatcher stages are not ordinary amplitude-runner stages.
inline std::optional<EnvelopeRunner> ContinueVoiceAmplitude(const VoiceStopState& voice,
    const EnvelopeRunner& prepared) noexcept
{
    const auto code = voice.stages[0];
    if (code != 22 && (code > 12 || (code & 1))) return std::nullopt;
    auto state = prepared.state();
    state.segment.stage = code == 22 ? EnvelopeStage::finished : EnvelopeStage(code/2);
    state.segment.progress.position = voice.progress;
    state.delayAccumulator = voice.delayAccumulator;
    state.pcmWord = voice.cached18;
    return EnvelopeRunner(prepared.setup(),state);
}

// 57c5..5809, after channel selection and preceding PCM setup. Caller must
// have passed the readiness gate. No key-mask update or sample setup here.
template<class Write>
void ActivatePreparedVoice(VoiceStopState& state,Write&& write)
{
    const auto word = [&](uint8_t address,uint16_t value) {
        write(address,uint8_t(value>>8)); write(uint8_t(address+1),uint8_t(value));
    };
    if (state.flagMinus3B & 128)
    {
        uint16_t command = state.cached18, value10 = state.pcm10;
        if (state.delayAccumulator == 0)
        { state.progress = 0; command = 0xb6; value10 = 0; }
        state.stages.fill(state.delayAccumulator == 0 ? 0 : 2);
        word(0x18,command);
        state.cached18 = command;
        word(0x10,value10);
    }
    else
    {
        word(0x18,state.cached18); word(0x10,state.pcm10);
        state.stages[0] = state.savedStage;
        state.savedStage = 0;
    }
}

// Prepared values, not ROM pointers. Register names deliberately avoid assigning
// unverified synthesis semantics to the firmware's cached control words.
struct PreparedVoicePcm
{
    SampleAddressSetup sample;
    uint16_t pcm12 = 0, pcm14 = 0, pcm1c = 0, pcm1a = 0;
};

enum class VoicePcmUpdateResult { invalidChannel, idle, written };

// 5855..5898. Gate on the FIRST envelope's raw stage, not pitch stage.
// cached1a is voice+26, distinct from prepared.pcm1a used at activation.
// The single voice scheduler owns channel selection and all ordered I/O.
template<class Write>
VoicePcmUpdateResult UpdateVoicePcm(uint8_t channel,const VoiceStopState& voice,
    const PreparedVoicePcm& prepared,uint16_t cached1a,Write&& write)
{
    if (channel >= 24) return VoicePcmUpdateResult::invalidChannel;
    if (voice.stages[0] == 0 || voice.stages[0] >= 14) return VoicePcmUpdateResult::idle;
    const auto word = [&](uint8_t address,uint16_t value) {
        write(address,uint8_t(value>>8)); write(uint8_t(address+1),uint8_t(value));
    };
    write(uint8_t(0x3e),channel);
    word(0x18,voice.cached18); word(0x16,voice.cached16);
    word(0x12,prepared.pcm12); word(0x14,prepared.pcm14);
    word(0x1a,cached1a); word(0x1c,prepared.pcm1c);
    word(0x10,voice.pcm10);
    return VoicePcmUpdateResult::written;
}

// voice68 supplies the HIGH byte of PCM1c as well as the output ceiling.
// Preserve voice66 (the low byte); activation's pcm1a must not replace +26.
template<class Write>
VoicePcmUpdateResult UpdateVoicePcm(uint8_t channel,const VoiceStopState& voice,
    const PreparedVoicePcm& prepared,const SecondEnvelopePcmState& second,Write&& write)
{
    auto mapped = prepared;
    mapped.pcm1c = uint16_t((uint16_t(second.control)<<8)|(prepared.pcm1c&255));
    return UpdateVoicePcm(channel,voice,mapped,second.command,write);
}

// Keep the TVA cache at its owner; do not require the scheduler to manually
// copy it into a second persistent cached16 field before every device write.
template<class Write>
VoicePcmUpdateResult UpdateVoicePcm(uint8_t channel,const VoiceStopState& voice,
    const PreparedVoicePcm& prepared,const TvaState& tva,const SecondEnvelopePcmState& second,Write&& write)
{
    auto mapped = voice;
    mapped.cached16 = tva.command;
    return UpdateVoicePcm(channel,mapped,prepared,second,write);
}

template<class Write>
VoicePcmUpdateResult UpdateVoicePcm(uint8_t channel,const VoiceStopState& voice,
    const PreparedVoicePcm& prepared,const VoiceOutputState& output,const SecondEnvelopePcmState& second,Write&& write)
{
    auto mapped = prepared;
    mapped.pcm12 = output.spatial.panWord; mapped.pcm14 = output.spatial.effects;
    return UpdateVoicePcm(channel,voice,mapped,output.tva,second,write);
}

struct VoiceKeyMask
{
    uint32_t enabled = 0; // CB24/CB26, preserve all firmware bits
    uint32_t prepared = 0; // CB28/CB2a, temporarily removed then re-enabled
};

struct VoicePostEnable
{
    uint8_t field65 = 0;
    uint16_t level = 0; // voice -16, written to PCM36 after logical right shift
    uint16_t command = 0; // voice +26, restored to PCM1a
};

// 57b1..57be / 5821..582b: activation uses -18/-16, whereas the
// post-enable command uses +26. Keep the already advanced periodic level
// intact. field65 and other PCM/sample preparation belong to their callers.
inline void PrepareVoiceSecondEnvelope(VoiceStopState& voice,PreparedVoicePcm& prepared,
    VoicePostEnable& post,const SecondEnvelopeReleaseState& segment,
    const SecondEnvelopeSetup& setup,const SecondEnvelopePcmState& pcm) noexcept
{
    voice.stages[1] = segment.stage;
    prepared.pcm1c = uint16_t((uint16_t(pcm.control)<<8)|setup.mode.pcmMode);
    prepared.pcm1a = setup.activation.command;
    post.level = setup.activation.level;
    post.command = pcm.command;
}

// Activation changes the raw stage only; it does not rewind the one-tick
// initialization, clear second-envelope progress, or replace its PCM cache.
inline std::optional<SecondEnvelopeReleaseState> ContinueVoiceSecondEnvelope(
    const VoiceStopState& voice,const SecondEnvelopeReleaseState& prepared) noexcept
{
    if (voice.stages[1] > 22 || (voice.stages[1]&1)) return std::nullopt;
    auto result = prepared;
    result.stage = voice.stages[1];
    return result;
}

// One bounded iteration of 580a..582d. false means PCM1e bit5 is not ready;
// nullopt is an invalid slot. The firmware spins here; a native scheduler must
// advance PCM time before retrying. No control writes are made while pending.
// Reselect on retry because the shared PCM channel may have changed.
template<class Read,class Write>
std::optional<bool> PollVoicePostEnable(unsigned channel,const VoicePostEnable& state,
    Read&& read,Write&& write)
{
    if (channel >= 24) return std::nullopt;
    if (state.field65 != 0) return true;
    write(0x3e,uint8_t(channel));
    (void)read(0x1e);
    const auto hi = read(0x3a); const auto lo = read(0x3b);
    if ((((uint16_t(hi)<<8)|lo) & 32) == 0) return false;
    const auto word = [&](uint8_t address,uint16_t value) {
        write(address,uint8_t(value>>8)); write(uint8_t(address+1),uint8_t(value));
    };
    word(0x1a,0xff00); word(0x36,uint16_t(state.level>>1)); word(0x1a,state.command);
    return true;
}

// 573f..576f: cancellation leaves both the cache and device untouched. Reading
// PCM00 is required: it commits the pending hardware mask, not a dummy read.
// Caller serializes this with other voice operations; callbacks must not reenter.
template<class Read,class Write>
bool RemovePreparedVoiceKeys(VoiceKeyMask& mask,const VoiceStopState& voice,
    Read&& read,Write&& write)
{
    if (voice.fieldCAF4 != 0) return false;
    mask.enabled &= ~mask.prepared;
    for (unsigned i = 0; i < 4; ++i)
        write(uint8_t(i),uint8_t(mask.enabled >> (24-8*i)));
    (void)read(0);
    return true;
}

// 582e..5854, after prepared voices have been programmed. This also commits
// via PCM00 read, then clears the prepared batch (in that order).
template<class Read,class Write>
void EnablePreparedVoiceKeys(VoiceKeyMask& mask,Read&& read,Write&& write)
{
    mask.enabled |= mask.prepared;
    for (unsigned i = 0; i < 4; ++i)
        write(uint8_t(i),uint8_t(mask.enabled >> (24-8*i)));
    (void)read(0);
    mask.prepared = 0;
}

// Entire 5777..5809 commit, after readiness and key-mask handling. Single owner,
// nonthrowing/nonreentrant device callback; invalid slots have no side effects.
template<class Write>
bool CommitPreparedVoice(unsigned channel,VoiceStopState& state,
    const PreparedVoicePcm& prepared,Write&& write)
{
    if (channel >= 24) return false;
    const auto word = [&](uint8_t address,uint16_t value) {
        write(address,uint8_t(value>>8)); write(uint8_t(address+1),uint8_t(value));
    };
    state.fieldC8B3 = state.fieldCB30 = 0;
    write(0x3e,uint8_t(channel));
    WriteSampleAddressSetup(prepared.sample,write);
    word(0x12,prepared.pcm12); word(0x14,prepared.pcm14);
    word(0x1c,prepared.pcm1c); word(0x1a,prepared.pcm1a);
    word(0x16,state.cached16);
    ActivatePreparedVoice(state,write);
    return true;
}

// Native continuation for the prepared portion of 559e..55d7 / 560e..5632.
// Owns copied inputs and a cursor, not references into H8 RAM. The caller owns
// the same voice array, mask and PCM for its whole lifetime. Only one batch may
// own them; reserved slots must not be reassigned until this task terminates.
// During post-enable suspension firmware masks interrupts: the native owner
// must likewise prevent competing lifecycle/control mutations, while advancing
// PCM. This class does not provide the outer control clock or MIDI scheduler.
class PreparedVoiceBatch
{
public:
    struct Entry { uint8_t channel = 0; PreparedVoicePcm pcm; VoicePostEnable post; };
    enum class Status { idle, waitingForReuse, waitingForKeyLatch, complete, cancelled };

    bool begin(std::span<const Entry> entries,VoiceKeyMask& mask)
    {
        if (status_ == Status::waitingForReuse || status_ == Status::waitingForKeyLatch
            || entries.empty() || entries.size() > 2 || mask.prepared != 0) return false;
        uint32_t bits = 0;
        for (const auto& entry : entries)
        {
            if (entry.channel >= 24 || (bits & (1u<<entry.channel))) return false;
            bits |= 1u<<entry.channel;
        }
        count_ = unsigned(entries.size()); cursor_ = 0;
        for (unsigned i = 0; i < count_; ++i) entries_[i] = entries[i];
        mask.prepared = bits;
        status_ = Status::waitingForReuse;
        return true;
    }

    Status status() const { return status_; }

    template<class Read,class Write>
    Status advance(std::array<VoiceStopState,24>& voices,VoiceKeyMask& mask,Read&& read,Write&& write)
    {
        if (status_ == Status::waitingForReuse)
        {
            for (; cursor_ < count_; ++cursor_)
            {
                const auto channel = entries_[cursor_].channel;
                const auto ready = *PollVoiceReuse(channel,voices[channel].fieldCAF4,read,write);
                if (ready == VoiceReuseReadiness::cancelled) return status_ = Status::cancelled;
                if (ready == VoiceReuseReadiness::pending) return status_;
            }
            // H8 rechecks the first voice here, including after waiting on the
            // second. Cancellation does not clear the prepared mask itself.
            if (!RemovePreparedVoiceKeys(mask,voices[entries_[0].channel],read,write))
                return status_ = Status::cancelled;
            for (unsigned i = 0; i < count_; ++i)
                (void)CommitPreparedVoice(entries_[i].channel,voices[entries_[i].channel],entries_[i].pcm,write);
            EnablePreparedVoiceKeys(mask,read,write);
            if ((voices[entries_[0].channel].flagMinus3B & 128) == 0)
                return status_ = Status::complete;
            cursor_ = 0; status_ = Status::waitingForKeyLatch;
        }
        if (status_ == Status::waitingForKeyLatch)
        {
            for (; cursor_ < count_; ++cursor_)
                if (!*PollVoicePostEnable(entries_[cursor_].channel,entries_[cursor_].post,read,write)) return status_;
            status_ = Status::complete;
        }
        return status_;
    }

private:
    std::array<Entry,2> entries_{};
    unsigned count_ = 0, cursor_ = 0;
    Status status_ = Status::idle;
};

// Compose 1a3b/1a53 with 53e6 and allocator reclaim. Return the group's
// original successor (R5), including ff. No wait for PCM ramp completion.
// State and device I/O must have one serialized owner. Callbacks must not
// throw or reenter/mutate these objects. Validate the entire reclaim first so
// bad links produce no device writes or partial state updates.
template<class Read,class Write>
std::optional<uint8_t> StopAndReclaimGroup(VoiceAllocator& allocator,
    std::array<VoiceStopState,24>& voices, unsigned group, unsigned part,
    bool prepend, Read&& read, Write&& write)
{
    if (group >= 24 || part >= 16) return std::nullopt;
    const auto successor = allocator.groupNext[group];
    auto checked = allocator;
    std::array<uint8_t,24> order{};
    unsigned count = 0;
    uint32_t seen = 0;
    do
    {
        const auto voice = checked.groups.tail[group];
        if (voice >= 24 || count == order.size() || (seen & (1u<<voice))) return std::nullopt;
        seen |= 1u<<voice;
        order[count++] = voice;
        if (!checked.reclaimStoppedVoice(voice,group,part,prepend)) return std::nullopt;
    } while (checked.groups.tail[group] < 128);

    for (unsigned i = 0; i < count; ++i)
    {
        const auto voice = order[i];
        auto& state = voices[voice];
        (void)StopPreparedVoice(voice,state,read,write); // validated logical slot
        state.fieldCAF4 = 4;
        // Validated above, no external allocator mutation is permitted.
        (void)allocator.reclaimStoppedVoice(voice,group,part,prepend);
    }
    return successor;
}

// 0ca5..0cb3 +16a4..16cc: selector0 bypasses the choke, otherwise stop ALL
// same-part groups with matching A2D0. Status, note number, hold and retained
// keys do not inhibit this forced stop. Reclaim appends voices to the free
// list immediately; PCM ramps/task4 still have to finish before slot reuse.
// Returns the number of groups stopped. Preflight the complete traversal with
// private state and inert I/O, so bad links cannot partially stop hardware.
// Callbacks must not throw or reenter/mutate the serialized voice owner.
template<class Read,class Write>
std::optional<unsigned> StopRhythmExclusiveGroups(VoiceAllocator& allocator,
    std::array<VoiceStopState,24>& voices,unsigned part,uint8_t selector,Read&& read,Write&& write)
{
    if (part >= 16) return std::nullopt;
    if (selector == 0) return 0;
    const auto visit = [&](auto& state,auto& life,auto&& load,auto&& store) -> std::optional<unsigned> {
        unsigned count = 0;
        uint32_t seen = 0;
        for (auto group = state.partHead[part]; group < 128;)
        {
            if (group >= 24 || (seen&(1u<<group))) return std::nullopt;
            seen |= 1u<<group;
            if (state.groupFieldA2D0[group] != selector) { group = state.groupNext[group]; continue; }
            const auto next = StopAndReclaimGroup(state,life,group,part,false,load,store);
            if (!next) return std::nullopt;
            ++count; group = *next;
        }
        return count;
    };
    auto checked = allocator; auto checkedVoices = voices;
    if (!visit(checked,checkedVoices,[](uint8_t) { return uint8_t(0); },[](uint8_t,uint8_t) {})) return std::nullopt;
    return visit(allocator,voices,read,write);
}

// 17b8..185c, BEFORE capacity preparation: optional repeated-key retirement.
// Mode0 stops the first same-key group, ignoring the selector, and prepends
// its slots. Mode1 skips retained keys; otherwise it marks matching groups
// with A288 bit2, stopping/appending the first one already marked. Other modes
// or disabled bit7 do nothing. Returns whether a group was physically stopped.
template<class Read,class Write>
std::optional<bool> RetireRepeatedNote(VoiceAllocator& allocator,
    std::array<VoiceStopState,24>& voices,unsigned part,uint8_t note,uint8_t selector,
    uint8_t partNoteFlags,std::span<const uint8_t,16> retained,Read&& read,Write&& write)
{
    if (part >= 16 || note > 127) return std::nullopt;
    const auto mode = partNoteFlags&3;
    if (!(partNoteFlags&128) || mode > 1) return false;
    if (mode == 1)
        for (const auto key : retained)
        {
            if (key&128) break;
            if (key == note) return false;
        }
    const auto visit = [&](auto& state,auto& life,auto&& load,auto&& store) -> std::optional<bool> {
        uint32_t seen = 0;
        for (auto group = state.partHead[part]; group < 128; group = state.groupNext[group])
        {
            if (group >= 24 || (seen&(1u<<group))) return std::nullopt;
            seen |= 1u<<group;
            if (state.groupValue[group] != note || (mode == 1 && state.groupFieldA2D0[group] != selector)) continue;
            if (mode == 1)
            {
                const bool marked = (state.groupFieldA288[group]&4) != 0;
                state.groupFieldA288[group] |= 4;
                if (!marked) continue;
            }
            if (!StopAndReclaimGroup(state,life,group,part,mode == 0,load,store)) return std::nullopt;
            return true;
        }
        return false;
    };
    auto checked = allocator; auto checkedVoices = voices;
    if (!visit(checked,checkedVoices,[](uint8_t) { return uint8_t(0); },[](uint8_t,uint8_t) {})) return std::nullopt;
    return visit(allocator,voices,read,write);
}

struct VoiceCapacityPolicy
{
    std::array<uint8_t,16> reserves{}, modes{}; // 8018, A040
    uint8_t startPartControl = 0; // 8028
    bool forceOldest = false; // current patch +5 bit4
};

// 1737..17b7, 18da..1960. This is capacity preparation, not note creation.
// nullopt indicates corrupt state/policy; false means valid policy could not
// free enough voices. Successful earlier reclaims remain committed on failure.
template<class Read,class Write>
std::optional<bool> EnsureVoiceCapacity(VoiceAllocator& allocator,
    std::array<VoiceStopState,24>& voices, unsigned incomingPart,unsigned requested,
    const VoiceCapacityPolicy& policy,Read&& read,Write&& write)
{
    if (incomingPart >= 16 || requested < 1 || requested > 2 || policy.startPartControl >= 16
        || allocator.freeCount > 24) return std::nullopt;
    for (unsigned p = 0; p < 16; ++p)
        if (policy.reserves[p] > 24 || allocator.partVoiceCount[p] > 24) return std::nullopt;
    if (requested <= allocator.freeCount) return true; // leave old shortage intact
    allocator.shortage = uint8_t(requested-allocator.freeCount);
    const auto enough = [&]() { return allocator.shortage == 0 || allocator.shortage >= 128; };
    const auto reclaimPart = [&](unsigned part) -> std::optional<bool> {
        const auto mode = policy.forceOldest ? 0 : policy.modes[part];
        if (mode != 0 && mode != 2) return false;
        const auto protectedValue = allocator.partMinimum[part]; // R4 retained across passes
        const unsigned passCount = mode == 0 ? 1 : 3;
        for (unsigned pass = 0; pass < passCount; ++pass)
        {
            if (pass == 2 && protectedValue >= 128) break;
            for (unsigned attempts = 0; attempts < 24; ++attempts)
            {
                unsigned group = allocator.partHead[part];
                if (mode == 2)
                {
                    const auto candidate = allocator.selectCandidate(part,protectedValue,
                        VoiceAllocator::CandidatePass(pass),allocator.activity);
                    if (!candidate) return std::nullopt;
                    if (candidate->voice == 255) break;
                    group = allocator.voiceGroup[candidate->voice];
                }
                if (group >= 128) break;
                if (group >= 24 || !StopAndReclaimGroup(allocator,voices,group,part,false,read,write)) return std::nullopt;
                if (enough()) return true;
                if (allocator.partVoiceCount[part] <= policy.reserves[part]) return false;
            }
        }
        return false;
    };
    unsigned part = policy.startPartControl != 0 && incomingPart <= policy.startPartControl
        ? policy.startPartControl : 15;
    uint32_t visited = 0;
    while (part < 128)
    {
        if (part >= 16 || (visited & (1u<<part))) return std::nullopt;
        visited |= 1u<<part;
        if (allocator.partVoiceCount[part] > policy.reserves[part])
        {
            const auto result = reclaimPart(part);
            if (!result || *result) return result;
        }
        part = allocator.partPrevious[part];
    }
    // Final attempt on the incoming part can cross its reserve by one group.
    if (allocator.shortage <= allocator.partVoiceCount[incomingPart]) return reclaimPart(incomingPart);
    return false;
}
}
