#pragma once
#include "sc55_control_random.h"
#include "sc55_voice_setup.h"
#include "sc55_tables.h"
#include "sc55_envelope.h"
#include "sc55_envelope_setup.h"

namespace sc55
{
struct PartPitchBase
{
    uint32_t pitch;
    uint32_t reference;
    uint32_t alternateReference;
};

// 486a..48d0. Descriptor tuning words are centered at 0400; the second
// reference subtracts its tuning from the first, not directly from key*1000.
inline PartPitchBase PreparePartPitchBase(uint8_t partKey,uint16_t partTune,
    uint8_t sampleKey,uint16_t sampleTune,uint16_t alternateTune) noexcept
{
    const uint32_t reference = (uint32_t(sampleKey)*1000+1024-sampleTune)&0xffffff;
    return {uint32_t(partKey)*1000+partTune,reference,
        (reference+1024-alternateTune)&0xffffff};
}

// Pitch correction arithmetic used by 4927..4955 and 4966..49aa. Negative
// corrections clamp on the wrapped 24-bit sign, positive corrections wrap.
inline uint32_t AdjustPartPitch(uint32_t pitch,int32_t adjustment) noexcept
{
    const uint32_t result = (pitch+uint32_t(adjustment))&0xffffff;
    return adjustment < 0 && (result & 0x800000) ? 0 : result;
}

// 5368..53e4. Signed depth sources saturate only when they have the same
// sign; their magnitude sum and doubled waveform retain H8 word wrapping.
inline uint32_t ApplyPitchModulation(uint32_t pitch,uint16_t first,
    uint16_t second,uint16_t waveform) noexcept
{
    bool negative;
    uint16_t magnitude;
    if ((first&0x8000) == (second&0x8000))
    {
        negative = (first&0x8000) != 0;
        magnitude = negative ? uint16_t(0u-first-second) : uint16_t(first+second);
        if (magnitude > 6000) magnitude = 6000;
    }
    else
    {
        magnitude = uint16_t(first+second);
        negative = (magnitude&0x8000) != 0;
        if (negative) magnitude = uint16_t(0u-magnitude);
    }
    if (waveform&0x8000)
    {
        waveform = uint16_t(0u-waveform);
        negative = !negative;
    }
    const uint16_t doubled = uint16_t(waveform*2u);
    const uint32_t amount = (uint32_t(magnitude)*doubled+32768u)>>16;
    const uint32_t result = (pitch+(negative ? 0u-amount : amount))&0xffffff;
    // Preserve the negative branch even when rounding makes amount zero.
    return negative && (result&0x800000) ? 0 : result;
}

struct PitchModulationInputs
{
    uint16_t offset = 0; // voice86, signed word
    struct Source { uint16_t first = 0, second = 0, waveform = 0; };
    std::array<Source,2> sources{};
    uint16_t masterTune = 1024;
    uint16_t partTune = 0; // signed word
};

// 50cf..5175. Does not modify the envelope's unmodulated output copy.
inline uint32_t PrepareModulatedPitch(uint32_t pitch,const PitchModulationInputs& input) noexcept
{
    pitch &= 0xffffff;
    if (input.offset&0x8000)
    {
        const uint16_t magnitude = uint16_t(0u-input.offset);
        pitch = pitch < magnitude ? 0 : pitch-magnitude;
    }
    else
    {
        pitch = (pitch+input.offset)&0xffffff;
        if (pitch > 127000) pitch = 127000;
    }
    for (const auto& source : input.sources)
        pitch = ApplyPitchModulation(pitch,source.first,source.second,source.waveform);
    const auto signedWord = [](uint16_t value) {
        return value&0x8000 ? int32_t(value)-65536 : int32_t(value);
    };
    pitch = AdjustPartPitch(pitch,signedWord(uint16_t(input.masterTune-1024u)));
    return AdjustPartPitch(pitch,signedWord(input.partTune));
}

inline uint32_t ApplyPartPitchFine(uint32_t pitch,uint8_t parameter) noexcept
{
    return AdjustPartPitch(pitch,(int32_t(parameter)-64)*10);
}

// 48f9..491f: selected key-correction table word is centered at 8000.
// A zero table selector bypasses this operation entirely (48dd).
inline uint32_t ApplyPartPitchKeyCorrection(uint32_t pitch,uint16_t tableWord) noexcept
{
    return AdjustPartPitch(pitch,int32_t(tableWord)-32768);
}

// v1.21 pointer directory 03:dd32..dd81. Expanded rows preserve byte-key
// addressing (including keys above MIDI 127); no firmware pointers at runtime.
using PartPitchKeyTables = std::array<std::array<uint16_t,256>,40>;

inline std::optional<uint32_t> ApplyPartPitchKeyTable(uint32_t pitch,uint8_t selector,
    uint8_t key,const PartPitchKeyTables& tables) noexcept
{
    if (selector == 0) return pitch & 0xffffff;
    if (selector >= tables.size()) return std::nullopt;
    return ApplyPartPitchKeyCorrection(pitch,tables[selector][key]);
}

// The input is the high byte of the first PCM34 read (channel 30), interpreted
// as signed. The second read has a separate cached byte and is not this input.
inline uint32_t ApplyPartPitchRandom(uint32_t pitch,uint8_t randomByte,uint8_t depth) noexcept
{
    const int32_t random = randomByte < 128 ? int32_t(randomByte) : int32_t(randomByte)-256;
    const unsigned magnitude = unsigned(random < 0 ? -random : random);
    const int32_t adjustment = int32_t((magnitude*depth+128)>>8)*10;
    const uint32_t result = (pitch+uint32_t(random < 0 ? -adjustment : adjustment))&0xffffff;
    // 4998 tests the result even when depth/rounding made the subtraction zero.
    // Preserve the original random sign, not the scaled adjustment's sign.
    return random < 0 && (result & 0x800000) ? 0 : result;
}

struct PartPitchPreparation
{
    uint32_t pitch; // copied to voice 28:3c and 6e:78
    uint8_t cachedRandom; // voice -3c, from the second PCM read
};

struct PitchEnvelopeDepthTables
{
    // Expanded byte-parameter addressing; supported musical ranges are a
    // separate import/controller constraint, not inferred from these sizes.
    std::array<uint16_t,192> base{}, velocity{};
    std::array<uint16_t,256> depth{};
    bool operator==(const PitchEnvelopeDepthTables&) const = default;
};

struct PitchEnvelopeTables
{
    PitchEnvelopeDepthTables depth;
    std::array<uint8_t,256> curve{};
    bool operator==(const PitchEnvelopeTables&) const = default;
};

struct PitchEnvelopeDepth
{
    uint16_t depth;
    std::array<uint8_t,5> offsets; // encoded signed bytes
};

// 4a5a..4ada. Center five levels at 64, invert them for negative sensitivity.
// Velocity multiplication uses its LOW word, then the depth product is rounded.
inline PitchEnvelopeDepth PreparePitchEnvelopeDepth(std::span<const uint8_t,92> partial,
    uint8_t velocity,const PitchEnvelopeDepthTables& tables) noexcept
{
    PitchEnvelopeDepth result{tables.depth[partial[0x10]],{}};
    const int sensitivity = int(partial[0x22])-64;
    for (unsigned i = 0; i < result.offsets.size(); ++i)
    {
        const uint8_t offset = uint8_t(unsigned(partial[0x12+i])-64);
        result.offsets[i] = sensitivity < 0 ? uint8_t(0u-offset) : offset;
    }
    if (sensitivity != 0)
    {
        const unsigned magnitude = unsigned(sensitivity < 0 ? -sensitivity : sensitivity);
        const uint16_t factor = uint16_t(tables.base[magnitude]+uint32_t(velocity)*tables.velocity[magnitude]);
        result.depth = uint16_t((uint32_t(result.depth)*factor+32768u)>>16);
    }
    return result;
}

// 4ada..4b23 (repeated for the other four pitch-envelope targets). The
// signed byte has already been centered/inverted upstream. Table lookup is
// explicit owned data; zero scaled magnitude still retains the sign branch.
inline uint32_t PreparePitchEnvelopeTarget(uint32_t base,uint8_t encodedOffset,
    uint16_t depth,std::span<const uint8_t,256> curve) noexcept
{
    const bool negative = (encodedOffset & 128) != 0;
    const uint8_t magnitude = negative ? uint8_t(0u-encodedOffset) : encodedOffset;
    // MUL's low-word high byte survives MOV.B before SWAP: keep it too.
    const uint32_t adjustment = (uint32_t(depth)*curve[magnitude]>>7)&65535;
    const uint32_t result = (base+(negative ? 0u-adjustment : adjustment))&0xffffff;
    return negative && (result & 0x800000) ? 0 : result;
}

struct PitchEnvelopeTargets
{
    uint16_t depth;
    std::array<uint32_t,5> pitch;
    uint8_t direction; // voice -3: 0 if first < second, otherwise 2
};

// 5060..50cf, one active segment only. Stage dispatch remains the caller's
// responsibility. The firmware interpolates a WRAPPED 16-bit distance even
// though the stored start/target/output are 24-bit values.
struct PitchEnvelopeSegment
{
    EnvelopeProgress progress{};
    uint32_t start = 0, target = 0;
    uint16_t increment = 0;
    uint8_t direction = 0;

    // 5019..5040. Does not reset progress: the completion dispatcher does
    // that separately and preserves deferred ticks for the next segment.
    // Equal endpoints choose direction0 here (initial preparation chooses2).
    void retarget(uint32_t nextTarget,uint16_t nextIncrement) noexcept
    {
        start = target & 0xffffff;
        target = nextTarget & 0xffffff;
        increment = nextIncrement;
        direction = target < start ? 2 : 0;
    }

    uint32_t advance(uint16_t ticks) noexcept
    {
        const uint16_t elapsed = uint16_t(ticks+progress.deferredTicks);
        const uint32_t position = uint32_t(elapsed)*increment+progress.position;
        progress.deferredTicks = 0;
        if (position > 65535)
        {
            progress.deferredTicks = uint16_t((position-65535)/increment);
            progress.position = 65535;
        }
        else progress.position = uint16_t(position);
        const uint16_t distance = direction == 0 ? uint16_t(target-start) : uint16_t(start-target);
        const uint32_t amount = (uint32_t(distance)*progress.position)>>16;
        return (start+(direction == 0 ? amount : 0u-amount))&0xffffff;
    }
};

// 4fdb..50cf, before modulation. Raw stage codes are retained until all
// preparation/release entry paths have been connected. Single state owner.
struct PitchEnvelopeRunner
{
    uint16_t stage = 2;
    PitchEnvelopeSegment segment;
    // Future destinations: voice6c:74,6d:76,6e:78 (last is base pitch).
    std::array<uint32_t,3> nextTargets{};
    std::array<uint16_t,3> nextIncrements{};
    uint32_t releaseTarget = 0;
    uint16_t releaseIncrement = 0;
    uint32_t output = 0;
    enum class Result { invalidStage, idle, updated };

    // Caller has consumed the voice request and synchronized the first
    // envelope. Supply its pre-release stage, NOT this pitch stage.
    VoiceReleaseAction requestRelease(uint16_t firstStage,uint16_t delayIncrement) noexcept
    {
        const auto action = SelectVoiceReleaseAction(firstStage,delayIncrement);
        if (action == VoiceReleaseAction::release) release();
        else if (action == VoiceReleaseAction::start) stage = 2;
        else if (action == VoiceReleaseAction::cancel) stage = 22;
        return action;
    }

    // Active note-off branch: stage store at 3269 and pitch setup 32a3..32dc.
    // The voice dispatcher must first check the amplitude stage/delay gate.
    // Unlike re-entry, release starts at the CURRENT UNMODULATED output.
    void release() noexcept
    {
        stage = 12;
        segment.target = output;
        segment.retarget(releaseTarget,releaseIncrement);
        segment.progress = {};
    }

    // 4f9e entry: recompute this stage's endpoints without advancing the
    // stage or resetting elapsed progress. This is not a MIDI note-off API.
    Result reenter(uint16_t ticks) noexcept
    {
        if (stage > 22 || (stage&1)) return Result::invalidStage;
        if (stage == 4)
            segment.retarget(nextTargets[0],nextIncrements[0]);
        else if (stage == 6)
        {
            segment.target = nextTargets[0];
            segment.retarget(nextTargets[1],nextIncrements[1]);
        }
        else if (stage == 8 || stage == 10)
        {
            segment.target = nextTargets[1];
            segment.retarget(nextTargets[2],nextIncrements[2]);
        }
        else if (stage >= 12)
        {
            segment.target = nextTargets[2];
            segment.retarget(releaseTarget,releaseIncrement);
        }
        return updateOutput(ticks);
    }

    Result advance(uint16_t ticks) noexcept
    {
        if (stage > 22 || (stage&1)) return Result::invalidStage;
        if (segment.progress.position == 65535)
        {
            segment.progress.position = 0;
            stage = stage < 12 ? uint16_t(stage+2) : uint16_t(22);
            if (stage >= 4 && stage <= 8)
            {
                const unsigned index = (stage-4)/2;
                segment.retarget(nextTargets[index],nextIncrements[index]);
            }
        }
        return updateOutput(ticks);
    }

private:
    Result updateOutput(uint16_t ticks) noexcept
    {
        if (stage == 10 || stage == 22)
        {
            segment.progress.deferredTicks = 0;
            output = segment.target&0xffffff;
        }
        else if ((stage >= 2 && stage <= 8) || stage == 12)
            output = segment.advance(ticks);
        else return Result::idle;
        return Result::updated;
    }
};

// 4a5a..4cb7. First target also initializes voice 2d:46 and 2c:44;
// caller installs it into its accumulator/segment state. No PCM reads here:
// cachedRandom is the second read retained by PreparedPartPitch.
inline PitchEnvelopeTargets PreparePitchEnvelopeTargets(uint32_t base,
    std::span<const uint8_t,92> partial,uint8_t velocity,uint8_t cachedRandom,
    const PitchEnvelopeDepthTables& tables,std::span<const uint8_t,256> curve) noexcept
{
    const auto prepared = PreparePitchEnvelopeDepth(partial,velocity,tables);
    PitchEnvelopeTargets result{prepared.depth,{},0};
    for (unsigned i = 0; i < result.pitch.size(); ++i)
        result.pitch[i] = PreparePitchEnvelopeTarget(base,prepared.offsets[i],prepared.depth,curve);
    result.pitch[0] = ApplyPartPitchRandom(result.pitch[0],cachedRandom,partial[0x11]);
    result.direction = result.pitch[0] < result.pitch[1] ? 0 : 2;
    return result;
}

// 4927..49aa. Single PCM owner; callbacks must preserve transaction ordering.
// Both complete word reads are intentional even though only high bytes survive.
// A clock-aware bus may advance PCM between reads; do not reuse the first value.
template<class Read, class Write>
PartPitchPreparation PreparePartPitch(uint32_t pitch,uint8_t fine,uint8_t depth,
    Read&& read,Write&& write)
{
    pitch = ApplyPartPitchFine(pitch,fine);
    if constexpr(requires { write.randomWord(); }) {
        const auto random=uint8_t(write.randomWord()>>8);
        const auto cached=uint8_t(write.randomWord()>>8);
        return {ApplyPartPitchRandom(pitch,random,depth),cached};
    }
    else {
    write(0x3e,30);
    (void)read(0x34);
    const uint8_t random = read(0x3a);
    (void)read(0x3b);
    (void)read(0x34);
    const uint8_t cached = read(0x3a);
    (void)read(0x3b);
    return {ApplyPartPitchRandom(pitch,random,depth),cached};
    }
}

// Construct on the preparation thread. No ROM data, lazy initialization or
// floating-point/transcendental operations in lookup/conversion/update.
class PitchConversion
{
public:
    PitchConversion() noexcept
    {
        for (unsigned i = 0; i < coarse_.size(); ++i) coarse_[i] = PitchCoarse(int(i));
        for (unsigned i = 0; i < fine_.size(); ++i) fine_[i] = PitchFine(int(i));
    }

    uint16_t fromDelta(uint32_t encodedDelta) const noexcept
    {
        const uint32_t bits = encodedDelta & 0xffffff;
        const int32_t delta = bits < 0x800000 ? int32_t(bits) : int32_t(bits)-0x1000000;
        if (delta >= 0) return delta >= 12000 ? 65535 : lookup(uint32_t(delta));
        const uint32_t magnitude = uint32_t(-delta);
        unsigned octave = magnitude/12000;
        unsigned remainder = magnitude%12000;
        if (remainder) { ++octave; remainder = 12000-remainder; }
        return octave >= 16 ? 0 : uint16_t(lookup(remainder)>>octave);
    }

    uint16_t correctionDivisor(uint32_t reference24) const noexcept
    {
        const auto value = fromDelta(reference24-81000u); // 528f: subtract 013c68
        return value == 0 ? 1 : value;
    }

private:
    uint16_t lookup(unsigned remainder) const noexcept
    {
        const uint32_t coarse = coarse_[remainder>>8];
        return uint16_t(coarse+((fine_[remainder & 255]*coarse)>>22));
    }
    std::array<uint16_t,47> coarse_{};
    std::array<uint32_t,256> fine_{};
};

struct PitchCorrectionCache
{
    uint8_t source = 0; // voice A4
    uint16_t offset = 0; // voice A6, encoded signed

    // 527c..5367. A changed reference alone does not refresh the correction:
    // preserve the firmware's source-byte-only invalidation condition.
    uint16_t update(uint8_t incoming,uint32_t reference24,uint16_t base,
        const PitchConversion& conversion) noexcept
    {
        if (source != incoming)
        {
            source = incoming;
            offset = *PreparePitchOffset(incoming,conversion.correctionDivisor(reference24));
        }
        return ApplyPitchOffset(base,offset);
    }
};

struct VoicePitch
{
    uint32_t accumulator = 0; // voice 2d:46, low 24 bits
    PitchCorrectionCache correction;

    // 51d5..5367. Increment has already been calculated by the upstream
    // modulation/portamento owner. All arithmetic wraps at 24 bits before
    // conversion; reference changes retain the source-only cache semantics.
    uint16_t advance(uint32_t increment24,uint32_t reference24,uint8_t source,
        const PitchConversion& conversion) noexcept
    {
        accumulator = (accumulator+increment24) & 0xffffff;
        const auto base = conversion.fromDelta(accumulator-reference24-12000u);
        return correction.update(source,reference24,base,conversion);
    }
};

// 519b..51d5: subtract the low 24 bits of elapsed*rate from the magnitude,
// clamp when the wrapped difference has its sign bit set, then restore sign.
// Do not substitute a wide signed max(0, magnitude-product): oversized products
// have observable 24-bit wrap semantics. The caller skips this when the stored
// increment is zero (5175..5189), before looking up rate.
inline uint32_t DecayPitchIncrement(uint32_t increment,uint16_t elapsed,uint16_t rate) noexcept
{
    constexpr uint32_t mask = 0xffffff;
    increment &= mask;
    const bool negative = (increment & 0x800000) != 0;
    const uint32_t magnitude = negative ? ((0u-increment)&mask) : increment;
    const uint32_t remaining = (magnitude-uint32_t(elapsed)*rate)&mask;
    if (remaining & 0x800000) return 0;
    return negative ? ((0u-remaining)&mask) : remaining;
}

using PitchGlideRates = std::array<uint16_t,128>;

struct PartPitchInputs
{
    uint8_t partKey;
    uint16_t partTune;
    uint8_t sampleKey;
    uint16_t sampleTune, alternateTune;
    uint8_t keyTable, fine, randomDepth, flags, sourceKey;
};

// 5175..5367: caller supplies owned data and the current part's rate index.
// The original pointer at voice-3a selected AB26[part]; no emulated pointer
// or control-ROM view is retained here. Rate data must outlive this call.
struct PitchGlide
{
    VoicePitch pitch;
    uint32_t increment = 0; // voice 2b:42, low 24 bits

    // 49aa..4a47, after the new part pitch has been calculated. Names retain
    // the raw flag/source distinction until the MIDI producer is connected.
    // No saturation: every sum/difference uses the firmware's 24-bit wrap.
    void prepare(uint8_t flags,uint8_t sourceKey,uint32_t previousPartPitch,
        uint32_t currentPartPitch) noexcept
    {
        constexpr uint32_t mask = 0xffffff;
        if (flags & 32)
        {
            increment = 0;
            if (sourceKey != 255)
            { increment = (uint32_t(sourceKey)*1000-currentPartPitch)&mask; return; }
            if (flags & 128) return;
        }
        const uint32_t difference = (previousPartPitch-currentPartPitch)&mask;
        if ((flags & 128) == 0) pitch.accumulator = (pitch.accumulator-difference)&mask;
        increment = (increment+difference)&mask;
    }

    std::optional<uint16_t> advance(uint16_t elapsed,uint8_t rateIndex,
        std::span<const uint16_t,128> rates,uint32_t reference24,uint8_t source,
        const PitchConversion& conversion) noexcept
    {
        const auto activeIncrement = increment & 0xffffff;
        if (activeIncrement != 0 && rateIndex >= rates.size()) return std::nullopt;
        // Zero increment bypasses the table access, including invalid indices.
        if (activeIncrement != 0) increment = DecayPitchIncrement(activeIncrement,elapsed,rates[rateIndex]);
        return pitch.advance(activeIncrement == 0 ? 0 : increment,reference24,source,conversion);
    }
};

// Single-owner update path from envelope dispatch through the final PCM pitch
// word (4fdb/4f9e..5367). Waveform/control producers run before this call.
struct VoicePitchRunner
{
    PitchEnvelopeRunner envelope;
    PitchGlide glide;
    uint16_t pcmWord = 0;
    enum class Result { invalidInput, idle, updated };

    // Install 4a5a..4f51's prepared results. Stage/progress, glide and cache
    // are deliberately retained: 4f51 selects initialization vs re-entry.
    void installEnvelope(uint32_t base,const PitchEnvelopeTargets& targets,
        const PitchEnvelopeTiming& timing) noexcept
    {
        envelope.segment.start = targets.pitch[0]&0xffffff;
        envelope.segment.target = targets.pitch[1]&0xffffff;
        envelope.segment.direction = targets.direction;
        envelope.segment.increment = timing.increments[0];
        envelope.nextTargets = {targets.pitch[2]&0xffffff,targets.pitch[3]&0xffffff,base&0xffffff};
        envelope.nextIncrements = {timing.increments[1],timing.increments[2],timing.increments[3]};
        envelope.releaseTarget = targets.pitch[4]&0xffffff;
        envelope.releaseIncrement = timing.increments[4];
        envelope.output = envelope.segment.start;
        glide.pitch.accumulator = envelope.output;
    }

    // 4f60..4f90: reset progress and cache key, then run one tick directly
    // through interpolation regardless of the current stage. The cache offset
    // itself is retained, including when the incoming correction source is 0.
    // The scheduler supplies the current part's rate, replacing H8's pointer.
    Result initialize(const PitchModulationInputs& modulation,uint8_t rateIndex,
        std::span<const uint16_t,128> rates,uint32_t reference,uint8_t correctionSource,
        const PitchConversion& conversion) noexcept
    {
        if ((glide.increment&0xffffff) != 0 && rateIndex >= rates.size()) return Result::invalidInput;
        envelope.segment.progress = {};
        glide.pitch.correction.source = 0;
        envelope.output = envelope.segment.advance(1);
        glide.pitch.accumulator = PrepareModulatedPitch(envelope.output,modulation);
        pcmWord = *glide.advance(1,rateIndex,rates,reference,correctionSource,conversion);
        return Result::updated;
    }

    Result advance(uint16_t ticks,bool reentry,const PitchModulationInputs& modulation,
        uint8_t rateIndex,std::span<const uint16_t,128> rates,uint32_t reference,
        uint8_t correctionSource,const PitchConversion& conversion) noexcept
    {
        auto nextEnvelope = envelope;
        const auto result = reentry ? nextEnvelope.reenter(ticks) : nextEnvelope.advance(ticks);
        if (result == PitchEnvelopeRunner::Result::invalidStage) return Result::invalidInput;
        if (result == PitchEnvelopeRunner::Result::idle)
        {
            envelope = nextEnvelope;
            return Result::idle;
        }
        if ((glide.increment&0xffffff) != 0 && rateIndex >= rates.size()) return Result::invalidInput;
        envelope = nextEnvelope;
        // Start afresh from the envelope every tick: never accumulate previous
        // modulation or a previous tick's glide addition into the next tick.
        glide.pitch.accumulator = PrepareModulatedPitch(envelope.output,modulation);
        pcmWord = *glide.advance(ticks,rateIndex,rates,reference,correctionSource,conversion);
        return Result::updated;
    }
};

// Per-voice owner of 485c..4a47's persistent results, not shared part scratch.
// The control scheduler must serialize this operation with PCM access.
struct PreparedPartPitch
{
    PartPitchBase values{};
    uint8_t cachedRandom = 0;
    PitchGlide glide;

    template<class Read,class Write>
    bool prepare(const PartPitchInputs& input,const PartPitchKeyTables* tables,
        Read&& read,Write&& write)
    {
        auto base = PreparePartPitchBase(input.partKey,input.partTune,input.sampleKey,
            input.sampleTune,input.alternateTune);
        if (input.keyTable != 0)
        {
            if (!tables) return false;
            const auto adjusted = ApplyPartPitchKeyTable(base.pitch,input.keyTable,input.partKey,*tables);
            if (!adjusted) return false;
            base.pitch = *adjusted;
        }
        // Validate before any PCM transaction or persistent state mutation.
        const auto adjusted = PreparePartPitch(base.pitch,input.fine,input.randomDepth,read,write);
        base.pitch = adjusted.pitch;
        glide.prepare(input.flags,input.sourceKey,values.pitch,base.pitch);
        values = base;
        cachedRandom = adjusted.cachedRandom;
        return true;
    }
};
}
