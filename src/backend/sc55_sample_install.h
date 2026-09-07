#pragma once
#include "sc55_note_setup.h"
#include "sc55_note_start.h"

namespace sc55
{
struct PartialSampleInstallInputs
{
    std::array<uint8_t,12> scale;
    uint8_t initialKey, sourceKey, originalNote, minimumKey, mode, remappedNote, sampleMode, flags;
};

// Fresh rhythm preparation at0cb9..0cc6 ->11fe ->1219. Both minimum-key
// slots are ff. Each partial restores the initial pitch but A1B2 is shared:
// a prepared first partial may replace the original note before the second
// partial's scale-tuning lookup. A skipped partial must not perform that write.
// Sample modes and preparation flags remain explicit caller-owned inputs.
inline std::optional<std::array<PartialSampleInstallInputs,2>> PrepareRhythmSampleInputs(
    const MelodicAllocationResult& allocation,const RhythmKeyMap& map,
    uint8_t partShift,uint8_t partOffset,const std::array<uint8_t,12>& scale,
    const std::array<uint8_t,2>& sampleModes,const std::array<uint8_t,2>& flags) noexcept
{
    if (allocation.status != MelodicAllocationResult::Status::allocated || !allocation.selection
        || !allocation.group) return std::nullopt;
    const auto key = allocation.selection->note;
    if (key >= 128 || map.tones[key] != allocation.selection->tone || map.pitches[key] >= 128) return std::nullopt;
    const auto keys = PrepareRhythmInitialKeys(key,map,partShift,partOffset);
    if (!keys || keys->initialKey >= 128 || keys->sourceKey >= 128) return std::nullopt;
    for (const auto value : scale) if (value >= 128) return std::nullopt;
    std::array<PartialSampleInstallInputs,2> result{};
    auto original = key;
    for (unsigned partial = 0; partial < 2; ++partial)
    {
        result[partial] = {scale,keys->initialKey,keys->sourceKey,original,255,
            map.groups[key],map.pitches[key],sampleModes[partial],flags[partial]};
        if (allocation.dispatch[partial].prepare && map.groups[key] != 0x80 && map.groups[key] != 0x81)
            original = map.pitches[key];
    }
    return result;
}

struct InstalledPartialSample
{
    uint8_t slot;
    PartialSamplePlan sample;
    // Absent when there is no physical destination or the negative sample
    // path returned the allocated voice instead of installing new metadata.
    std::optional<InstalledVoice> installed;
};
using InstalledPartialSamples = std::array<std::optional<InstalledPartialSample>,2>;

// Sample selection and 125d/12e5 ->53e6/113a for an allocated melodic note.
// The caller resolves keys/mode/scale and owns prior-key caches; no GS defaults
// or mono semantics are inferred. Preserve partial order and slot fallback.
// Validate both sample plans and installation transactions before PCM I/O.
// No key-on, readiness wait, task dispatch or DSP preparation is performed.
// After I/O starts, callbacks must not throw/reenter/mutate these owners.
template<class Read,class Write>
std::optional<InstalledPartialSamples> PrepareAndInstallMelodicSamples(
    const MelodicAllocationResult& allocation,unsigned part,
    const std::array<PartialSampleInstallInputs,2>& inputs,const SoundData& data,
    VoiceAllocator& allocator,VoiceInstallationState& installation,
    std::array<VoiceStopState,24>& lifecycle,Read&& read,Write&& write)
{
    if (allocation.status != MelodicAllocationResult::Status::allocated || !allocation.selection
        || !allocation.group || allocation.group->group >= 24 || part >= 16 || !data.samples())
        return std::nullopt;
    const auto& selection = *allocation.selection;
    const auto* patch = data.patch(selection.tone);
    if (!patch) return std::nullopt;
    const auto dispatch = PlanPartialVoiceDispatch(*patch,selection.partials.candidates.flags,
        {allocation.group->voices[0],allocation.group->voices[1]});
    if (!dispatch) return std::nullopt;
    InstalledPartialSamples result{};
    std::array<VoiceInstallationInput,2> requests{};
    auto checkedAllocator = allocator;
    auto checkedInstallation = installation;
    for (unsigned partial = 0; partial < 2; ++partial)
    {
        const auto destination = (*dispatch)[partial];
        if (destination.prepare != allocation.dispatch[partial].prepare
            || destination.voice != allocation.dispatch[partial].voice) return std::nullopt;
        if (!destination.prepare) continue;
        const auto& input = inputs[partial];
        if (input.initialKey >= 128 || input.sourceKey >= 128 || input.originalNote >= 128
            || input.remappedNote >= 128) return std::nullopt;
        // minimumKey is a byte with a high-bit "no minimum" sentinel, not MIDI.
        const auto sample = PreparePartialSample(patch->partial[partial],*data.samples(),input.initialKey,
            input.sourceKey,input.originalNote,input.scale,input.minimumKey,input.mode,input.remappedNote);
        if (!sample) return std::nullopt;
        const auto slot = destination.voice;
        result[partial] = InstalledPartialSample{slot,*sample,{}};
        if (slot >= 128) continue;
        if (slot >= 24 || allocator.voicePart[slot] != part
            || allocator.voiceGroup[slot] != allocation.group->group) return std::nullopt;
        requests[partial] = {selection.tone,sample->sampleId,uint8_t(partial),uint8_t(part),
            sample->key.storedOriginalNote.value_or(input.originalNote),
            sample->key.storedAdjustedKey.value_or(input.initialKey),selection.velocity,
            input.sampleMode,sample->key.lookupKey,(input.flags&128) != 0};
        auto flags = input.flags;
        if (!checkedInstallation.install(slot,requests[partial],flags,checkedAllocator)) return std::nullopt;
    }
    for (unsigned partial = 0; partial < 2; ++partial)
    {
        if (!result[partial] || result[partial]->slot >= 128) continue;
        const auto slot = result[partial]->slot;
        auto flags = inputs[partial].flags;
        if (!RestartAndInstallVoice(slot,requests[partial],flags,allocator,installation,lifecycle[slot],read,write))
            return std::nullopt; // Prevalidated; only an invalid callback can invalidate this transaction.
        if (!(result[partial]->sample.sampleId&0x8000)) result[partial]->installed = installation.voices[slot];
    }
    return result;
}
}
