#pragma once
#include "sc55_patch.h"
#include "sc55_sample_bank.h"
#include "sc55_pitch.h"
#include "sc55_envelope_runner.h"
#include "sc55_sound_data.h"

namespace sc55
{
// Result of partial pitch -> key resolution -> sample selection. This is not
// voice allocation/key-on; special sample IDs must take their own later path.
struct PartialSamplePlan
{
    TrackedKey pitch;
    PartialKeyResolution key;
    uint16_t sampleId;
    std::optional<SampleAddresses> normalStart, unoffsetStart;
};

struct PartialSamplePcm
{
    SampleAddressSetup address;
    uint8_t loopFlag = 0;
};

// 2c5d..2fcc: the sample descriptor's first byte is a level, not address
// data. Keep its attenuation tied to the selected sample rather than allowing
// a caller's neutral placeholder. Special sample IDs need a separate path.
inline std::optional<EnvelopeRunner> PreparePartialAmplitude(
    const SC55Patch& patch,unsigned partialIndex,const PartialSamplePlan& plan,
    const SampleBank& samples,uint8_t envelopeKey,uint8_t amplitude,
    EnvelopeStage allocatedStage,uint8_t attackControl,const EnvelopeLevelTables& levels,
    const EnvelopeKeyLevelTables& keyLevels,const EnvelopeKeyTables& keys,const EnvelopeTimes& times) noexcept
{
    if (partialIndex >= 2 || !patch.partial[partialIndex].used || (plan.sampleId&0x8000)) return std::nullopt;
    const auto* sample = samples.sample(plan.sampleId);
    if (!sample) return std::nullopt;
    return EnvelopeRunner::fromKey(patch.partial[partialIndex],
        {envelopeKey,amplitude,sample->data[0],patch.common[0],allocatedStage,attackControl},levels,keyLevels,keys,times);
}

// Bridge a selected normal sample to 485c's inputs. Caller owns note/part
// controls and must use a plan and patch from the same sound-data generation.
inline std::optional<PartPitchInputs> PreparePartialPitchInputs(
    const SC55Patch& patch,unsigned partialIndex,const PartialSamplePlan& plan,
    const SampleBank& samples,uint8_t partKey,uint16_t partTune,uint8_t flags,uint8_t sourceKey) noexcept
{
    if (partialIndex >= 2 || !patch.partial[partialIndex].used || (plan.sampleId & 0x8000)) return std::nullopt;
    const auto* sample = samples.sample(plan.sampleId);
    if (!sample) return std::nullopt;
    const auto& data = sample->data;
    const auto& partial = patch.partial[partialIndex];
    return PartPitchInputs{partKey,partTune,data[11],uint16_t((data[12]<<8)|data[13]),
        uint16_t((data[14]<<8)|data[15]),patch.common[7],partial.raw[11],partial.raw[12],flags,sourceKey};
}

struct NormalPitchStartInputs
{
    uint8_t partKey, envelopeKey, velocity;
    uint16_t partTune;
    uint8_t flags, sourceKey, glideRate, correctionSource;
    PitchModulationInputs modulation;
};

struct PreparedNormalPitch
{
    // Preparation snapshot. On a later note, combine values with the live
    // runner's glide state; this snapshot does not advance with the voice.
    PreparedPartPitch part;
    VoicePitchRunner runner;
};

// Compose normal-sample pitch preparation (485c..4f90) from owned MD14
// tables. The caller supplies keys explicitly: envelopeKey is not necessarily
// partKey, and partTune is the tracked fractional pitch, not master tuning.
// Previous per-voice pitch/glide state belongs to the caller; never replace
// it with zero merely because another note is starting. A continuing runner
// selects 4f51's re-entry branch and supplies LIVE progress/glide/cache state;
// nullptr selects the normal initialization branch. The caller chooses this
// from the installed voice's restart flag, not MIDI mono mode alone.
// Inputs/table availability are checked before the PCM random transaction.
// No allocation, CPU, control ROM, key-on write, or clock advancement here.
template<class Read,class Write>
std::optional<PreparedNormalPitch> PrepareNormalVoicePitch(
    const SC55Patch& patch,unsigned partialIndex,const PartialSamplePlan& plan,
    const NormalPitchStartInputs& input,const PreparedPartPitch& previous,
    const SoundData& data,const PitchConversion& conversion,Read&& read,Write&& write,
    const VoicePitchRunner* continuing = nullptr,uint16_t elapsed = 1)
{
    if (!data.samples() || !data.pitchTiming() || !data.pitchEnvelope()
        || !data.keys() || !data.times() || !data.glideRates() || input.glideRate >= 128)
        return std::nullopt;
    const auto partInput = PreparePartialPitchInputs(patch,partialIndex,plan,*data.samples(),
        input.partKey,input.partTune,input.flags,input.sourceKey);
    if (!partInput) return std::nullopt;
    const auto& partial = patch.partial[partialIndex];
    const auto timing = PreparePitchEnvelopeTiming(partial.raw,input.envelopeKey,input.velocity,
        *data.pitchTiming(),data.keys()->multipliers,*data.times());
    if (!timing) return std::nullopt;
    if (continuing && (continuing->envelope.stage>22 || (continuing->envelope.stage&1)))
        return std::nullopt;
    PreparedNormalPitch result{previous,continuing ? *continuing : VoicePitchRunner{}};
    if (continuing) result.part.glide=continuing->glide;
    if (!result.part.prepare(*partInput,data.pitchKeys(),read,write)) return std::nullopt;
    const auto targets = PreparePitchEnvelopeTargets(result.part.values.pitch,partial.raw,input.velocity,
        result.part.cachedRandom,data.pitchEnvelope()->depth,data.pitchEnvelope()->curve);
    result.runner.glide = result.part.glide;
    result.runner.installEnvelope(result.part.values.pitch,targets,*timing);
    const auto state=continuing
        ? result.runner.advance(elapsed,true,input.modulation,input.glideRate,*data.glideRates(),
            result.part.values.reference,input.correctionSource,conversion)
        : result.runner.initialize(input.modulation,input.glideRate,*data.glideRates(),
            result.part.values.reference,input.correctionSource,conversion);
    if (state == VoicePitchRunner::Result::invalidInput)
        return std::nullopt;
    return result;
}

// Bridge a resolved native sample plan to the exact PCM startup input. Special
// sample IDs are not normal descriptors and remain an explicit unsupported
// branch. History is caller-owned state, never silently assumed to be zero.
inline std::optional<PartialSamplePcm> PreparePartialSamplePcm(
    const PartialSamplePlan& plan,const SampleBank& samples,unsigned channel,
    bool unoffsetStart,uint8_t historyNibble) noexcept
{
    if (channel >= 24 || (plan.sampleId & 0x8000)) return std::nullopt;
    const auto* descriptor = samples.sample(plan.sampleId);
    const auto& addresses = unoffsetStart ? plan.unoffsetStart : plan.normalStart;
    if (!descriptor || !addresses) return std::nullopt;
    const auto control = DecodeSampleControl(addresses->loop,descriptor->data[10],uint8_t(channel),historyNibble);
    return PartialSamplePcm{{control.mode,addresses->start,addresses->loop,addresses->end},control.loopFlag};
}

// Inputs are note/part state and owned sound data, never CPU registers or ROM
// pointers. Missing/unsupported data is distinct from a valid special sample.
inline std::optional<PartialSamplePlan> PreparePartialSample(
    const SC55Partial& partial, const SampleBank& samples,
    uint8_t initialKey, uint8_t sourceKey, uint8_t originalNote,
    std::span<const uint8_t,12> scale, uint8_t minimumKey,
    uint8_t mode, uint8_t remappedNote) noexcept
{
    if (!partial.used) return std::nullopt;
    const auto pitch = PreparePartialPitch(initialKey,sourceKey,originalNote,partial.raw,scale);
    if (!pitch) return std::nullopt;
    const auto key = ResolvePartialKey(pitch->key,minimumKey,mode,remappedNote,partial.raw[1]);
    const uint16_t group = uint16_t((partial.raw[2] << 8) | partial.raw[3]);
    const auto id = samples.select(group,key.lookupKey);
    if (!id) return std::nullopt;
    PartialSamplePlan plan{*pitch,key,*id,{},{}};
    if (!(*id & 0x8000))
    {
        const auto* sample = samples.sample(*id);
        if (!sample) return std::nullopt;
        const auto descriptor = std::span(sample->data).first<10>();
        plan.normalStart = DecodeSampleAddresses(descriptor,false);
        plan.unoffsetStart = DecodeSampleAddresses(descriptor,true);
    }
    return plan;
}
}
