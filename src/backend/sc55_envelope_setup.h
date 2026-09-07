#pragma once
#include "sc55_second_envelope_tables.h"
#include "sc55_patch.h"
#include "sc55_envelope.h"
#include <array>
#include <optional>

namespace sc55
{
enum class EnvelopeStage : uint8_t { delay, attack1, attack2, decay1, decay2, sustain, release, finished };
struct EnvelopeStageState
{
    EnvelopeStage stage;
    EnvelopeProgress progress;
    SC55ExpandedParameter parameter;
    uint8_t start, target;
};

// v1.21 33f4..3445: finishing a segment advances the stage and, for
// attack2/decay1/decay2, loads the next parameter/target. Deferred time
// survives the transition; reaching sustain alone does not request release.
inline EnvelopeStageState AdvanceEnvelopeStage(EnvelopeStageState state,
    std::span<const SC55ExpandedParameter,5> parameters,std::span<const uint8_t,4> targets) noexcept
{
    if (state.stage == EnvelopeStage::finished || state.progress.position != 65535) return state;
    state.progress.position = 0;
    state.stage = static_cast<EnvelopeStage>(static_cast<unsigned>(state.stage)+1);
    const unsigned stage = static_cast<unsigned>(state.stage);
    if (stage >= 2 && stage <= 4)
    {
        state.parameter = parameters[stage-1];
        state.start = state.target;
        state.target = targets[stage-1];
    }
    return state;
}

enum class VoiceReleaseAction { unchanged, start, cancel, release };
// 3220..3269. All three envelopes use the first envelope's stage to decide.
inline VoiceReleaseAction SelectVoiceReleaseAction(uint16_t firstStage,uint16_t delayIncrement) noexcept
{
    if (firstStage >= 12) return VoiceReleaseAction::unchanged;
    if (firstStage == 0) return delayIncrement == 0 ? VoiceReleaseAction::start : VoiceReleaseAction::cancel;
    return VoiceReleaseAction::release;
}

// v1.21 3220..328b, the first envelope's release request. The caller has
// already consumed the pending request and synchronized the PCM level.
// Delay cancellation differs from ordinary release and preserves its fields.
inline EnvelopeStageState ReleaseEnvelope(EnvelopeStageState state,uint16_t currentLevel,
    SC55ExpandedParameter releaseParameter,uint16_t delayIncrement) noexcept
{
    const auto action = SelectVoiceReleaseAction(uint16_t(unsigned(state.stage)*2),delayIncrement);
    if (action == VoiceReleaseAction::unchanged) return state;
    if (action != VoiceReleaseAction::release)
    {
        state.stage = action == VoiceReleaseAction::start ? EnvelopeStage::attack1 : EnvelopeStage::finished;
        return state;
    }
    state.stage = EnvelopeStage::release;
    state.progress = {0,0};
    state.start = uint8_t(currentLevel >> 8);
    state.target = 0;
    state.parameter = releaseParameter;
    return state;
}

using EnvelopeTimes = std::array<uint16_t,128>;

struct EnvelopeKeyTables
{
    std::array<uint16_t,256> multipliers;
    std::array<std::array<uint8_t,256>,16> attack, release;
};

// v1.21 2d83..2dcc / 2de7..2e30. Negative sensitivity reverses the
// unsigned curve point, with zero specially becoming 255 rather than zero.
inline std::optional<uint16_t> EnvelopeKeyScale(uint8_t point,uint8_t sensitivity,
    const std::array<uint16_t,256>& multipliers) noexcept
{
    if (sensitivity < 44 || sensitivity > 84) return std::nullopt;
    if (sensitivity == 64) return 256;
    const unsigned magnitude = sensitivity < 64 ? 64-sensitivity : sensitivity-64;
    if (sensitivity < 64) point = point == 0 ? uint8_t(255) : uint8_t(-point);
    const int delta = int(point)-128;
    if (delta == 0) return 256;
    const unsigned coefficient = (magnitude*128+10)/20;
    const int scaled = int((unsigned(delta < 0 ? -delta : delta)*coefficient*2) >> 8);
    return multipliers[unsigned(128+(delta < 0 ? -scaled : scaled))];
}
inline std::optional<std::array<uint16_t,2>> PrepareEnvelopeKeyScales(
    const SC55Partial& partial,uint8_t key,const EnvelopeKeyTables& tables) noexcept
{
    if (partial.raw[0x55] >= 16 || partial.raw[0x56] >= 16) return std::nullopt;
    const auto attack = EnvelopeKeyScale(tables.attack[partial.raw[0x55]][key],partial.raw[0x57],tables.multipliers);
    const auto release = EnvelopeKeyScale(tables.release[partial.raw[0x56]][key],partial.raw[0x58],tables.multipliers);
    if (!attack || !release) return std::nullopt;
    return std::array<uint16_t,2>{*attack,*release};
}

struct PitchEnvelopeKeyCurves
{
    std::array<std::array<uint8_t,256>,16> attack, release;
};

// 4cbb..4d8c: pitch uses its own curve directories, but shares the amplitude
// envelope's multiplier table and sensitivity arithmetic. Attack selector's
// high nibble is ignored; release selector is not masked by the firmware.
inline std::optional<std::array<uint16_t,2>> PreparePitchEnvelopeKeyScales(
    std::span<const uint8_t,92> partial,uint8_t key,const PitchEnvelopeKeyCurves& curves,
    const std::array<uint16_t,256>& multipliers) noexcept
{
    if (partial[0x1f] >= 16) return std::nullopt;
    const auto attack = EnvelopeKeyScale(curves.attack[partial[0x1e]&15][key],partial[0x20],multipliers);
    const auto release = EnvelopeKeyScale(curves.release[partial[0x1f]][key],partial[0x21],multipliers);
    if (!attack || !release) return std::nullopt;
    return std::array<uint16_t,2>{*attack,*release};
}

struct EnvelopeLevelTables
{
    std::array<uint8_t,128> attenuation;
    std::array<uint8_t,256> level;
};

struct EnvelopeKeyLevelTables
{
    std::array<uint8_t,129> adjustment;
    std::array<std::array<uint8_t,256>,16> curves;
};

// v1.21 2c6a..2cda: partial level and key-dependent bias. Positive
// sensitivity raises levels above the curve midpoint, negative reverses it.
inline std::optional<uint8_t> PrepareEnvelopeKeyLevel(const SC55Partial& partial,uint8_t key,
    const EnvelopeLevelTables& levels,const EnvelopeKeyLevelTables& keys) noexcept
{
    const auto level = partial.raw[0x45], selector = partial.raw[0x46], sensitivity = partial.raw[0x47];
    if (level > 127 || selector >= 16 || sensitivity < 44 || sensitivity > 84) return std::nullopt;
    const int base = levels.attenuation[level] == 255 ? 1 : 255-levels.attenuation[level];
    const int delta = int(keys.curves[selector][key])-128;
    const unsigned magnitude = sensitivity < 64 ? 64-sensitivity : sensitivity-64;
    const unsigned coefficient = (magnitude*128+10)/20;
    const auto index = (unsigned(delta < 0 ? -delta : delta)*coefficient*2) >> 8;
    const int adjustment = keys.adjustment[index];
    const bool subtract = (delta < 0) != (sensitivity < 64);
    const int result = base+(subtract ? -adjustment : adjustment);
    return uint8_t(result < 1 ? 1 : result > 255 ? 255 : result);
}

// v1.21 2cda..2d14. Input base is the result of partial/key scaling.
// The three independent subtractions each floor at ONE (not zero).
// amplitude is prepared partial velocity; recordLevel is the byte at the
// voice's +a0 record, and patchLevel is patch common[0].
inline std::optional<uint8_t> PrepareEnvelopeBase(uint8_t base,uint8_t amplitude,
    uint8_t recordLevel,uint8_t patchLevel,const EnvelopeLevelTables& tables) noexcept
{
    for (const auto value : {amplitude,recordLevel,patchLevel})
    {
        if (value > 127) return std::nullopt;
        const auto attenuation = tables.attenuation[value];
        base = base <= attenuation ? uint8_t(1) : uint8_t(base-attenuation);
    }
    return base;
}

// v1.21 2d14..2d67. Base already includes the common/key/velocity
// attenuation. Segment levels use a zero floor, unlike the earlier base
// calculation's floor of one. Tables are owned sound data, not CPU memory.
inline std::optional<std::array<uint8_t,4>> PrepareEnvelopeTargets(
    const SC55Partial& partial,uint8_t base,const EnvelopeLevelTables& tables) noexcept
{
    std::array<uint8_t,4> result;
    for (unsigned i = 0; i < result.size(); ++i)
    {
        const auto parameter = partial.raw[0x4a+i];
        if (parameter > 127) return std::nullopt;
        const auto attenuation = tables.attenuation[parameter];
        result[i] = tables.level[base < attenuation ? 0 : base-attenuation];
    }
    return result;
}

// The firmware saturates when the product's high word reaches 255,
// before narrowing by eight bits. A generic 16-bit clamp differs near the top.
inline uint16_t ScaleEnvelopeTime(uint16_t time,uint16_t scale) noexcept
{
    const uint32_t product = uint32_t(time)*scale;
    return (product >> 16) >= 255 ? uint16_t(65535) : uint16_t(product >> 8);
}
inline std::optional<uint16_t> PrepareEnvelopeDuration(uint8_t stage,uint8_t controller,
    uint16_t keyScale,uint16_t velocityScale,const EnvelopeTimes& times) noexcept
{
    if (stage > 127 || controller > 127) return std::nullopt;
    const int adjusted = int(stage)+2*(int(controller)-64);
    const unsigned index = unsigned(adjusted < 0 ? 0 : adjusted > 127 ? 127 : adjusted);
    return ScaleEnvelopeTime(ScaleEnvelopeTime(times[index],keyScale),velocityScale);
}

enum class SecondEnvelopeInterval { attack, decay, release };
struct SecondEnvelopeTiming
{
    uint16_t keyScale = 256, releaseKeyScale = 256;
    uint16_t attackVelocityScale = 256, decayReleaseVelocityScale = 256;
    uint8_t attack = 64, decay = 64, release = 64;
    bool attackControlEnabled = false;

    // 44a3/44ff/4564..45b6. Only attack control is gated by voiceA2 bit4.
    std::optional<uint16_t> duration(uint8_t parameter,SecondEnvelopeInterval interval,
        const EnvelopeTimes& times) const noexcept
    {
        switch (interval)
        {
            case SecondEnvelopeInterval::attack:
                return PrepareEnvelopeDuration(parameter,attackControlEnabled ? attack : uint8_t(64),keyScale,attackVelocityScale,times);
            case SecondEnvelopeInterval::decay:
                return PrepareEnvelopeDuration(parameter,decay,keyScale,decayReleaseVelocityScale,times);
            case SecondEnvelopeInterval::release:
                return PrepareEnvelopeDuration(parameter,release,releaseKeyScale,decayReleaseVelocityScale,times);
        }
        return std::nullopt;
    }
};

// v1.21 00:2e30..2ed7, repeated for partial +59 and +5a.
// The input is the prepared partial amplitude, not raw MIDI velocity.
inline std::optional<uint16_t> EnvelopeVelocityScale(uint8_t amplitude,uint8_t sensitivity) noexcept
{
    if (sensitivity < 44 || sensitivity > 84) return std::nullopt;
    if (sensitivity == 64) return 256;
    const unsigned magnitude = sensitivity < 64 ? 64-sensitivity : sensitivity-64;
    // Exact 21-byte ROM table at 679a, derived arithmetically.
    const unsigned coefficient = (magnitude*128+10)/20;
    const unsigned product = uint8_t(127-amplitude)*coefficient;
    if (sensitivity < 64)
        return product >= 8001 ? uint16_t(4) : uint16_t(((8128-product)*2064) >> 16);
    const auto denominator = uint16_t(8128-product);
    return denominator < 32 ? uint16_t(65535) : uint16_t(2080768u/denominator);
}

// 425b..43d1. The normal/attack and release key curves have their own
// directories; they are not the amplitude or pitch key curves. Controllers
// and the attack enable flag retain neutral defaults for the caller to set.
inline std::optional<SecondEnvelopeTiming> PrepareSecondEnvelopeTiming(
    const SC55Partial& partial,uint8_t key,uint8_t amplitude,
    const SecondEnvelopeTimingKeyCurves& curves,const std::array<uint16_t,256>& multipliers) noexcept
{
    if (partial.raw[0x39] >= 16 || partial.raw[0x3a] >= 16) return std::nullopt;
    const auto attack = EnvelopeKeyScale(curves.attack[partial.raw[0x39]][key],partial.raw[0x3b],multipliers);
    const auto release = EnvelopeKeyScale(curves.release[partial.raw[0x3a]][key],partial.raw[0x3c],multipliers);
    const auto attackVelocity = EnvelopeVelocityScale(amplitude,partial.raw[0x3e]);
    const auto decayReleaseVelocity = EnvelopeVelocityScale(amplitude,partial.raw[0x3f]);
    if (!attack || !release || !attackVelocity || !decayReleaseVelocity) return std::nullopt;
    SecondEnvelopeTiming result;
    result.keyScale = *attack; result.releaseKeyScale = *release;
    result.attackVelocityScale = *attackVelocity; result.decayReleaseVelocityScale = *decayReleaseVelocity;
    return result;
}

// 4de1..4e2a: pitch envelope stores reciprocal progress increments, unlike
// amplitude's duration. High parameter bit does not participate in lookup.
inline uint16_t PreparePitchEnvelopeIncrement(uint8_t parameter,uint16_t keyScale,
    uint16_t velocityScale,const EnvelopeTimes& times) noexcept
{
    const uint16_t duration = ScaleEnvelopeTime(ScaleEnvelopeTime(times[parameter&127],keyScale),velocityScale);
    return duration <= 8 ? uint16_t(65535) : uint16_t(524288u/duration);
}

// 4d8c..4e2a, first segment. Reuse the verified amplitude sensitivity formula;
// the pitch caller's input is C914, not necessarily the amplitude runner's value.
inline std::optional<uint16_t> PreparePitchEnvelopeFirstIncrement(
    std::span<const uint8_t,92> partial,uint8_t velocity,uint16_t keyScale,
    const EnvelopeTimes& times) noexcept
{
    const auto scale = EnvelopeVelocityScale(velocity,partial[0x23]);
    if (!scale) return std::nullopt;
    return PreparePitchEnvelopeIncrement(partial[0x17],keyScale,*scale,times);
}

struct PitchEnvelopeTiming
{
    std::array<uint16_t,2> keyScales;
    uint16_t velocityScale;
    std::array<uint16_t,5> increments;
};

// 4cbb..4f51: first four intervals use the attack key scale; the fifth
// uses release key scale. All five use the same velocity scale.
inline std::optional<PitchEnvelopeTiming> PreparePitchEnvelopeTiming(
    std::span<const uint8_t,92> partial,uint8_t key,uint8_t velocity,
    const PitchEnvelopeKeyCurves& curves,const std::array<uint16_t,256>& multipliers,
    const EnvelopeTimes& times) noexcept
{
    const auto keyScales = PreparePitchEnvelopeKeyScales(partial,key,curves,multipliers);
    const auto velocityScale = EnvelopeVelocityScale(velocity,partial[0x23]);
    if (!keyScales || !velocityScale) return std::nullopt;
    PitchEnvelopeTiming result{*keyScales,*velocityScale,{}};
    for (unsigned i = 0; i < result.increments.size(); ++i)
        result.increments[i] = PreparePitchEnvelopeIncrement(partial[0x17+i],(*keyScales)[i == 4 ? 1 : 0],*velocityScale,times);
    return result;
}

struct PartialEnvelopePlan
{
    std::array<SC55ExpandedParameter,5> stages;
    uint16_t velocityScale1, velocityScale2;
};
inline std::optional<PartialEnvelopePlan> PreparePartialEnvelope(
    const SC55Partial& partial,uint8_t amplitude) noexcept
{
    const auto first = EnvelopeVelocityScale(amplitude,partial.raw[0x59]);
    const auto second = EnvelopeVelocityScale(amplitude,partial.raw[0x5a]);
    if (!first || !second) return std::nullopt;
    PartialEnvelopePlan plan{{},*first,*second};
    for (unsigned i = 0; i < 5; ++i)
        plan.stages[i] = SC55_ExpandParameter(partial.raw[0x4e + i]);
    return plan;
}
}
