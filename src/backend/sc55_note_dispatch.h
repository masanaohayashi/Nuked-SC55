#pragma once
#include "sc55_channel.h"
#include "sc55_voice_operation.h"
#include "sc55_preset.h"
#include "sc55_sound_data.h"
#include "sc55_voice_allocator.h"
#include "sc55_rhythm.h"

namespace sc55
{
struct PreparedNoteVelocity
{
    uint16_t tone;
    uint8_t note, velocity;
    PatchVelocityPlan partials;
};
using MelodicNoteVelocity = PreparedNoteVelocity;

struct PartialVoiceDispatch
{
    bool prepare = false;
    uint8_t voice = 255; // High-bit sentinel: prepare but do not install.
};

struct PartialVoiceInputs
{
    uint16_t pitchFraction = 0;
    uint8_t amplitude = 0, secondary = 0, previousKey = 255;
    bool operator==(const PartialVoiceInputs&) const = default;
};

// Owned handoff between partial preparation and voice installation. Boot/reset
// must supply the appropriate previous-key state; defaults are not GS defaults.
// 1244..129b / 12c5..1323: a prepared partial updates its part's key cache even
// without a voice. Slot inputs are committed only after the restart delegate.
// This does not perform restart (53e6) or installation (113a).
struct PartialDispatchState
{
    std::array<std::array<uint8_t,2>,16> previousKeys{};
    std::array<PartialVoiceInputs,24> voices{};

    bool stage(unsigned part,unsigned partial,PartialVoiceDispatch dispatch,
               uint8_t adjustedKey,PartialVoiceInputs input) noexcept
    {
        if (part >= previousKeys.size() || partial >= 2
            || (dispatch.prepare && dispatch.voice >= voices.size() && dispatch.voice < 128)) return false;
        if (!dispatch.prepare) return true;
        previousKeys[part][partial] = adjustedKey;
        if (dispatch.voice < voices.size()) voices[dispatch.voice] = input;
        return true;
    }
};

struct VoiceInstallationInput
{
    // Identifiers into the caller-owned sound-data generation, never H8 pointers.
    uint16_t tone = 0, sample = 0;
    uint8_t partial = 0, part = 0, originalKey = 0, adjustedKey = 0;
    uint8_t velocity = 0, sampleMode = 0, sampleKey = 0;
    bool restarted = false;
    bool operator==(const VoiceInstallationInput&) const = default;
};

struct InstalledVoice
{
    VoiceInstallationInput input;
    uint8_t flags = 0;
    VoiceOperation operation = VoiceOperation::none;
};

// 113a..11cf, including the negative-sample return path. Data identity replaces
// patch/partial/descriptor bank+pointer pairs. Caller validates those IDs against
// its immutable sound data before normal installation. No PCM key-on here.
struct VoiceInstallationState
{
    std::array<InstalledVoice,24> voices{};
    std::array<uint8_t,24> pendingRelease{};

    bool install(unsigned slot,const VoiceInstallationInput& input,
                 uint8_t& preparationFlags,VoiceAllocator& allocator) noexcept
    {
        if (slot >= voices.size() || input.part >= 16 || input.partial >= 2) return false;
        if (input.sample&0x8000)
        {
            auto updated = allocator;
            updated.allocations[slot].status = 0;
            if (!updated.returnVoice(slot)) return false;
            allocator = updated;
            return true; // Old metadata, pending release and preparation flags survive.
        }
        if (input.restarted) preparationFlags |= 128;
        voices[slot] = {input,preparationFlags,VoiceOperation::prepare};
        allocator.allocations[slot].status = 0;
        allocator.allocations[slot].releaseCommand = 0;
        pendingRelease[slot] = 0;
        return true;
    }
};

// 1219..132a dispatch policy, not the delegated sample/voice preparation.
// Candidate bits and multisample ffff gate preparation. Partial1 (second)
// falls back to the first slot when its own slot is negative. Preserve order:
// both preparations may target the same slot; never deduplicate them.
inline std::optional<std::array<PartialVoiceDispatch,2>> PlanPartialVoiceDispatch(
    const SC55Patch& patch,uint8_t candidates,std::array<uint8_t,2> slots) noexcept
{
    std::array<PartialVoiceDispatch,2> result{};
    for (unsigned i = 0; i < 2; ++i)
    {
        const auto& raw = patch.partial[i].raw;
        if (!(candidates&(1u<<i)) || (raw[2] == 255 && raw[3] == 255)) continue;
        const auto voice = i == 1 && slots[1] >= 128 ? slots[0] : slots[i];
        if (voice < 128 && voice >= 24) return std::nullopt;
        result[i] = {true,voice};
    }
    return result;
}

// Shared patch-velocity expansion after tone routing and part adjustment.
// Global tone IDs retain bank2; unsupported/absent IDs never fall back.
inline std::optional<PreparedNoteVelocity> PrepareMappedNoteVelocity(
    const MidiDecoder::Event& event,unsigned tone,bool softPedal,uint8_t accumulator,const SoundData& data) noexcept
{
    if (event.kind != MidiDecoder::Kind::message || (event.status&0xf0) != 0x90
        || event.dataSize != 2 || event.first > 127 || event.second == 0 || event.second > 127)
        return std::nullopt;
    const auto* patch = data.patch(tone);
    if (!patch || !data.curves()) return std::nullopt;
    return PreparedNoteVelocity{uint16_t(tone),event.first,event.second,
        PreparePatchVelocity(patch->common[6],event.second,patch->partial[0].raw,patch->partial[1].raw,
            accumulator,softPedal,*data.curves())};
}

struct RhythmNoteVelocity
{
    RhythmToneSelection mapping;
    // Absent mapped tone is a valid no-note result. Present tone with zero
    // candidates is velocity rejection; missing patch data is invalid instead.
    std::optional<PreparedNoteVelocity> note;
};

// Call only after common/part receive gates with the already-adjusted event
// and selected map. Group/flags remain available for the later release and
// allocation stages; this function neither releases groups nor starts PCM.
inline std::optional<RhythmNoteVelocity> PrepareRhythmNoteVelocity(
    const MidiDecoder::Event& event,unsigned program,const RhythmKeyMap& map,
    std::span<const uint8_t,128> program127Accumulators,uint8_t accumulator,bool softPedal,
    const SoundData& data) noexcept
{
    if (event.kind != MidiDecoder::Kind::message || (event.status&0xf0) != 0x90
        || event.dataSize != 2 || event.first > 127 || event.second == 0 || event.second > 127)
        return std::nullopt;
    const auto mapping = SelectRhythmTone(event.first,program,map,program127Accumulators,accumulator);
    if (!mapping) return std::nullopt;
    if (!mapping->present()) return RhythmNoteVelocity{*mapping,{}};
    const auto note = PrepareMappedNoteVelocity(event,mapping->tone,softPedal,mapping->velocityAccumulator,data);
    if (!note) return std::nullopt;
    return RhythmNoteVelocity{*mapping,note};
}

// Entry to melodic note expansion, not allocation/key-on. Routing and the
// effective bank/rhythm selection must already have been resolved by caller;
// MIDI channel10 is not intrinsically a drum part under GS routing.
// No bank fallback, high-note remapping or drum substitution is hidden here.
// A zero-velocity Note On belongs to note-off handling and is rejected.
// A present result with zero candidates is a valid velocity rejection.
// This lower-level helper takes an already part-adjusted velocity. The routed
// allocation entry below applies21be exactly once before calling it.
inline std::optional<MelodicNoteVelocity> PrepareMelodicNoteVelocity(
    const MidiDecoder::Event& event,const ChannelControls::Channel& channel,
    unsigned bankMsb,bool rhythm,uint8_t accumulator,const SoundData& data) noexcept
{
    if (event.kind != MidiDecoder::Kind::message || (event.status&0xf0) != 0x90
        || event.dataSize != 2 || event.first > 127 || event.second == 0 || event.second > 127
        || rhythm) return std::nullopt;
    const auto tone = ResolveV121MelodicPreset(bankMsb,channel.program);
    if (!tone) return std::nullopt;
    return PrepareMappedNoteVelocity(event,*tone,channel.softPedal,accumulator,data);
}

// 21be..2207, part+0a/+0b. Parameters are 7-bit receive configuration.
// The multiply reads BOTH adjacent bytes as a big-endian word; offset also
// contributes to the fractional scale. A zero result is a rejected Note On,
// not a release. Depth0 substitutes1; depth64 skips multiplication entirely.
struct PartVelocityAdjustment
{
    uint8_t depth = 64, offset = 64;
};

inline std::optional<uint8_t> AdjustPartNoteVelocity(unsigned velocity,PartVelocityAdjustment input) noexcept
{
    if (velocity == 0 || velocity > 127 || input.depth > 127 || input.offset > 127)
        return std::nullopt;
    const unsigned scaled = input.depth == 0 ? 1 : input.depth == 64 ? velocity
        : (velocity*(unsigned(input.depth)*256+input.offset)/64)>>8;
    if (input.offset < 64)
    {
        const int shifted = int(scaled)-2*(64-int(input.offset));
        return uint8_t(shifted < 1 ? 1 : shifted > 127 ? 127 : shifted);
    }
    const unsigned shifted = scaled+2*(unsigned(input.offset)-64);
    return uint8_t(shifted > 127 ? 127 : shifted);
}

// 2150..215e, after the nonzero-velocity path. R4 is the original key.
// BTST reads a BYTE at AC0E: parts8..15 cannot bypass this comparison.
// Keep raw bounds (part+0c/+0d); inverted bounds reject, not normalize.
struct NoteKeyRange
{
    uint8_t low = 0, high = 127;
};

inline bool AcceptNoteKeyRange(unsigned part,unsigned key,NoteKeyRange range,uint8_t fieldAC0E) noexcept
{
    if (part >= 16 || key > 127) return false;
    return (unsigned(fieldAC0E)&(1u<<part)) || (key >= range.low && key <= range.high);
}

struct PartNoteOnInputs
{
    uint8_t noteFlags = 0; // part+5: bit4 rhythm, bit5 selects first map.
    // Caller supplies the two raw per-key bytes, not whole drum presets:
    // map0=8b48+key, map1=8fd4+key. Configuration ownership stays outside.
    std::array<uint8_t,2> rhythmKeyFlags{};
    PartVelocityAdjustment velocityAdjustment{};
    NoteKeyRange keyRange{};
    uint8_t fieldAC0E = 0;
};

struct PartNoteOnSelection
{
    enum class Status { invalidInput, release, rhythmRejected, velocityRejected, keyRangeRejected, prepare };
    Status status;
    uint8_t velocity = 0; // Effective velocity, valid only for prepare.
};

// 2126..2160 after common receive routing, including the21be delegate.
// Zero MIDI velocity takes218e BEFORE rhythm receive-enable, adjustment and
// key limits; adjusted zero instead takes2191 (ignore). No event consumption,
// allocation or PCM access. prepare still requires tone/capacity policy.
inline PartNoteOnSelection SelectPartNoteOn(unsigned part,unsigned key,unsigned velocity,
    const PartNoteOnInputs& input) noexcept
{
    using Status = PartNoteOnSelection::Status;
    if (part >= 16 || key > 127 || velocity > 127) return {Status::invalidInput};
    if (velocity == 0) return {Status::release};
    if ((input.noteFlags&0x10) && !(input.rhythmKeyFlags[(input.noteFlags&0x20) ? 0 : 1]&0x10))
        return {Status::rhythmRejected};
    const auto adjusted = AdjustPartNoteVelocity(velocity,input.velocityAdjustment);
    if (!adjusted) return {Status::invalidInput};
    if (*adjusted == 0) return {Status::velocityRejected};
    if (!AcceptNoteKeyRange(part,key,input.keyRange,input.fieldAC0E)) return {Status::keyRangeRejected};
    return {Status::prepare,*adjusted};
}

struct MelodicAllocationInputs
{
    unsigned bankMsb;
    bool rhythm;
    uint8_t velocityAccumulator, part, groupFlags, groupPriority;
    NoteKeyRange keyRange{};
    uint8_t fieldAC0E = 0;
    PartVelocityAdjustment velocityAdjustment{};
    std::optional<uint16_t> resolvedTone{}; // Program-change owner resolved the bank table.
    std::optional<uint8_t> groupNote{}; // High-note mapping keeps the original release key.
};

struct MelodicAllocationResult
{
    enum class Status { invalidInput, keyRangeRejected, velocityRejected, needsCapacity, allocated };
    Status status = Status::invalidInput;
    std::optional<MelodicNoteVelocity> selection;
    std::optional<VoiceAllocator::GroupAllocation> group;
    std::array<PartialVoiceDispatch,2> dispatch{};
};

// One already-routed melodic Note On: select tone/velocity, reserve its group
// and derive ordered partial destinations. No PCM access, sample preparation
// or hidden GS defaults. Capacity shortage is explicit: the engine must apply
// its capacity policy, not silently drop the note or retry forever.
// Only allocated commits allocator state. Rejected/invalid/shortage results
// leave it unchanged. This does not replace mono reuse, drums or part fan-out.
inline MelodicAllocationResult AllocateMelodicNote(
    const MidiDecoder::Event& event,const ChannelControls::Channel& channel,
    const MelodicAllocationInputs& input,const SoundData& data,VoiceAllocator& allocator) noexcept
{
    MelodicAllocationResult result;
    if (input.part >= 16 || allocator.freeCount > 24) return result;
    if (event.kind != MidiDecoder::Kind::message || (event.status&0xf0) != 0x90
        || event.dataSize != 2 || event.first > 127 || event.second == 0 || event.second > 127
        || input.rhythm) return result;
    const auto received = SelectPartNoteOn(input.part,event.first,event.second,
        {0,{},input.velocityAdjustment,input.keyRange,input.fieldAC0E});
    using Received = PartNoteOnSelection::Status;
    if (received.status == Received::velocityRejected)
    { result.status = MelodicAllocationResult::Status::velocityRejected; return result; }
    if (received.status == Received::keyRangeRejected)
    { result.status = MelodicAllocationResult::Status::keyRangeRejected; return result; }
    if (received.status != Received::prepare) return result;
    auto adjustedEvent = event; adjustedEvent.second = received.velocity;
    result.selection = input.resolvedTone
        ? PrepareMappedNoteVelocity(adjustedEvent,*input.resolvedTone,channel.softPedal,input.velocityAccumulator,data)
        : PrepareMelodicNoteVelocity(adjustedEvent,channel,input.bankMsb,input.rhythm,input.velocityAccumulator,data);
    if (!result.selection) return result;
    const auto count = result.selection->partials.candidates.count;
    if (count == 0) { result.status = MelodicAllocationResult::Status::velocityRejected; return result; }
    if (count > 2) return result;
    if (allocator.freeCount < count)
    { result.status = MelodicAllocationResult::Status::needsCapacity; return result; }
    auto updated = allocator;
    const auto group = updated.createGroup({input.part,input.groupFlags,input.groupNote.value_or(result.selection->note),
        input.groupPriority,uint8_t(count)});
    if (!group) return result;
    const auto dispatch = PlanPartialVoiceDispatch(*data.patch(result.selection->tone),
        result.selection->partials.candidates.flags,{group->voices[0],group->voices[1]});
    if (!dispatch) return result;
    result.group = group; result.dispatch = *dispatch;
    result.status = MelodicAllocationResult::Status::allocated;
    allocator = updated;
    return result;
}

// Existing mono group at0b1a/0b97 ->1219. Tone/velocity and restart policy
// are already resolved by the part owner. Never allocate extra voices to
// satisfy a changed partial count here: firmware reuses the existing slots.
// An explicit group selects CC84's polyphonic source instead of partHead.
inline MelodicAllocationResult PrepareMonoReuseAllocation(const MelodicNoteVelocity& selection,
    unsigned part,const SoundData& data,const VoiceAllocator& allocator,uint8_t group=255) noexcept
{
    MelodicAllocationResult result;
    if (part>=16 || selection.note>=128 || selection.velocity>=128) return result;
    const auto* patch=data.patch(selection.tone);
    if(group==255) group=allocator.partHead[part];
    const auto reuse=allocator.prepareGroupReuse(group,{0xff,{255,255}});
    if (!patch || !reuse) return result;
    for(const auto voice:reuse->voices)
        if(voice<24 && (allocator.allocations[voice].part!=part || allocator.allocations[voice].noteGroup!=group)) return result;
    result.selection=selection;
    if (!selection.partials.candidates.count) {
        result.status=MelodicAllocationResult::Status::velocityRejected; return result;
    }
    const auto dispatch=PlanPartialVoiceDispatch(*patch,selection.partials.candidates.flags,reuse->voices);
    if (!dispatch) return result;
    result.group=VoiceAllocator::GroupAllocation{group,{reuse->voices[0],reuse->voices[1],255}};
    result.dispatch=*dispatch; result.status=MelodicAllocationResult::Status::allocated;
    return result;
}

// Routing/receive selector and retained-key producer remain caller-owned.
// Do not publish pending requests here: the note-management batch owns that
// boundary. Both MIDI encodings of note-off select the same allocator path.
inline std::optional<bool> RequestMelodicNoteOff(const MidiDecoder::Event& event,
    unsigned part,uint8_t selector,std::span<const uint8_t,16> retainedKeys,VoiceAllocator& allocator) noexcept
{
    const auto kind = event.status&0xf0;
    if (event.kind != MidiDecoder::Kind::message || event.dataSize != 2
        || event.first > 127 || event.second > 127
        || (kind != 0x80 && !(kind == 0x90 && event.second == 0))) return std::nullopt;
    return allocator.requestNoteRelease(part,event.first,selector,retainedKeys);
}

// CC64, after MIDI-channel-to-part routing and receive permission resolution.
// A disabled receiver consumes this controller without changing any state.
// Pending-request publication remains at the caller's batch boundary.
inline bool ApplyRoutedHoldController(const MidiDecoder::Event& event,unsigned part,
    bool receiveEnabled,std::span<const uint8_t,16> retainedKeys,VoiceAllocator& allocator) noexcept
{
    if (event.kind != MidiDecoder::Kind::message || (event.status&0xf0) != 0xb0
        || event.dataSize != 2 || event.first != 64 || event.second > 127 || part >= 16) return false;
    return !receiveEnabled || allocator.setPartHold(part,event.second >= 64,retainedKeys);
}

// CC66: 228b ->2702, queued commands8/10 ->087d/0882. Every enabled
// message captures/releases, including repeated high values (not edge-only).
// receiveEnabled combines the routed part's receive bits11 and7. State is
// the caller-owned AB00 part bit; publication stays at the batch boundary.
inline bool ApplyRoutedSostenutoController(const MidiDecoder::Event& event,unsigned part,
    bool receiveEnabled,bool& enabled,std::span<uint8_t,16> retainedKeys,VoiceAllocator& allocator) noexcept
{
    if (event.kind != MidiDecoder::Kind::message || (event.status&0xf0) != 0xb0
        || event.dataSize != 2 || event.first != 66 || event.second > 127 || part >= 16) return false;
    if (!receiveEnabled) return true;
    const bool next = event.second >= 64;
    if (!(next ? allocator.captureRetainedKeys(part,retainedKeys)
               : allocator.releaseRetainedKeys(part,retainedKeys))) return false;
    enabled = next;
    return true;
}

struct PartMidiReceive
{
    // v1.21 part+4 and big-endian word at part+2. Values >=16 disable
    // channel matching. These are configuration data, not ROM addresses.
    uint8_t channel = 255;
    uint16_t flags = 0;
    uint8_t noteFlags = 0; // part+5; includes rhythm and poly/mono mode.
};

struct NoteReleaseSelection
{
    enum class Path { group, monophonic };
    Path path;
    uint8_t selector;
};

// 0a19..0a5d. This selects a release path, not drum data or mono retrigger.
// Rhythm wins over both high-key and poly mode; high-key wins over mono.
inline std::optional<NoteReleaseSelection> SelectNoteRelease(unsigned key,uint8_t flags) noexcept
{
    if (key > 127) return std::nullopt;
    if (flags&0x10) return NoteReleaseSelection{NoteReleaseSelection::Path::group,0};
    if (key >= 125) return NoteReleaseSelection{NoteReleaseSelection::Path::group,0x81};
    if (flags&0x80) return NoteReleaseSelection{NoteReleaseSelection::Path::group,0x80};
    return NoteReleaseSelection{NoteReleaseSelection::Path::monophonic,0};
}

// Shared note-on/off receive gate at2076..209c and20fe..2126.
// Raw mode fields retain neutral names until their UI/GS producers are mapped.
struct NoteReceiveMode
{
    uint8_t fieldCDCC = 0, fieldCDF4 = 0, fieldCDF5 = 0;
};

inline bool AcceptNotePart(unsigned channel,unsigned part,const PartMidiReceive& receive,
    NoteReceiveMode mode) noexcept
{
    if (channel >= 16 || part >= 16 || receive.channel != channel) return false;
    if (mode.fieldCDCC&8)
        return (mode.fieldCDCC&2) || mode.fieldCDF4 == part;
    return !(mode.fieldCDF5&1) && (receive.flags&0x0200);
}

struct MonoHeldKeys
{
    // 0984/09a0 update one bit in eight16-bit words. Repeated note-on is
    // idempotent, not a count.09bc scans highest key first, not last-played.
    std::array<uint16_t,8> words{};

    bool set(unsigned key,bool down) noexcept
    {
        if (key >= 128) return false;
        const auto bit = uint16_t(1u<<(key%16));
        if (down) words[key/16] |= bit;
        else words[key/16] &= uint16_t(~bit);
        return true;
    }

    std::optional<uint8_t> highest() const noexcept
    {
        for (unsigned key = 128; key-- > 0;)
            if (words[key/16]&(1u<<(key%16))) return uint8_t(key);
        return std::nullopt;
    }

    struct ReleaseDecision
    {
        enum class Action { unchangedVoice, releaseGroup, replaceKey };
        Action action;
        uint8_t replacement = 255;
    };

    // 0a64..0a74: clear the released bit even for a noncurrent note. Only
    // releasing A070's current key selects a replacement/stop. This does not
    // update currentKey: replacement preparation and voice commit own that.
    std::optional<ReleaseDecision> release(unsigned key,uint8_t currentKey) noexcept
    {
        if (!set(key,false)) return std::nullopt;
        if (key != currentKey) return ReleaseDecision{ReleaseDecision::Action::unchangedVoice};
        if (const auto next = highest()) return ReleaseDecision{ReleaseDecision::Action::replaceKey,*next};
        return ReleaseDecision{ReleaseDecision::Action::releaseGroup};
    }
};

// 0a74..0a87: replacement re-evaluates the selected patch with a fresh
// accumulator and current preparation velocity (A3D3), NOT the replacement
// key's original note-on velocity. A zero-candidate plan is a valid rejection
// and requires the caller's mono group-release branch, not a fallback tone.
inline std::optional<PatchVelocityPlan> PrepareMonoReplacementVelocity(
    MonoHeldKeys::ReleaseDecision decision,const SC55Patch& patch,uint8_t preparationVelocity,
    bool softPedal,const VelocityCurves& curves) noexcept
{
    if (decision.action != MonoHeldKeys::ReleaseDecision::Action::replaceKey
        || decision.replacement > 127 || preparationVelocity > 127) return std::nullopt;
    return PreparePatchVelocity(patch.common[6],preparationVelocity,
        patch.partial[0].raw,patch.partial[1].raw,0,softPedal,curves);
}

// Serialized note-management owner. Part indices are resolved routing, not
// MIDI channel indices. Retained rows live as long as the allocator, never
// per voice or per callback. Construction does not reset active PCM voices;
// initialize allocator tables separately during instrument preparation.
class PartNoteState
{
public:
    struct Part
    {
        std::array<uint8_t,16> retainedKeys;
        bool sostenutoEnabled = false;
        MonoHeldKeys monoHeldKeys;
        Part() noexcept { retainedKeys.fill(255); }
    };

    VoiceAllocator allocator;

    const Part* part(unsigned index) const noexcept
    { return index < parts.size() ? &parts[index] : nullptr; }

    bool applyPedal(const MidiDecoder::Event& event,unsigned index,bool receiveEnabled) noexcept
    {
        if (index >= parts.size()) return false;
        auto& state = parts[index];
        return ApplyRoutedHoldController(event,index,receiveEnabled,state.retainedKeys,allocator)
            || ApplyRoutedSostenutoController(event,index,receiveEnabled,state.sostenutoEnabled,
                state.retainedKeys,allocator);
    }

    bool allNotesOff(unsigned index,bool rhythm) noexcept
    {
        return index < parts.size()
            && allocator.requestGroupReleases(index,rhythm,0x80,0,parts[index].retainedKeys);
    }

    // 08ba: hold-off then retained-key release; do not clear mono held keys.
    bool resetPedals(unsigned index) noexcept
    {
        if (index >= parts.size()) return false;
        auto updated = *this;
        auto& part = updated.parts[index];
        if (!updated.allocator.setPartHold(index,false,part.retainedKeys)
            || !updated.allocator.releaseRetainedKeys(index,part.retainedKeys)) return false;
        part.sostenutoEnabled = false;
        *this = updated;
        return true;
    }

    // 268a/2702 visit parts15..0, matching channel and BOTH receive gates.
    // Fan-out is intentional. Configuration is explicit: no hardcoded GS
    // default map. Invalid state rolls back the complete fan-out operation.
    bool receivePedal(const MidiDecoder::Event& event,std::span<const PartMidiReceive,16> routing) noexcept
    {
        if (event.kind != MidiDecoder::Kind::message || (event.status&0xf0) != 0xb0
            || event.dataSize != 2 || (event.first != 64 && event.first != 66)
            || event.second > 127) return false;
        const uint16_t mask = uint16_t(0x0800 | (event.first == 64 ? 0x0020 : 0x0080));
        auto updated = *this;
        for (unsigned index = 16; index-- > 0;)
            if (routing[index].channel == (event.status&15) && (routing[index].flags&mask) == mask)
                if (!updated.applyPedal(event,index,true)) return false;
        *this = updated;
        return true;
    }

    std::optional<bool> noteOff(const MidiDecoder::Event& event,unsigned index,uint8_t selector) noexcept
    {
        if (index >= parts.size()) return std::nullopt;
        return RequestMelodicNoteOff(event,index,selector,parts[index].retainedKeys,allocator);
    }

    // Receive fan-out plus group selection. Result bits identify parts with
    // matching groups, not just enabled receivers. nullopt includes unsupported mono
    // retrigger: never fall back to selector0. No partial fan-out on failure.
    std::optional<uint16_t> receiveNoteOff(const MidiDecoder::Event& event,
        std::span<const PartMidiReceive,16> routing,NoteReceiveMode mode) noexcept
    {
        const auto kind = event.status&0xf0;
        if (event.kind != MidiDecoder::Kind::message || event.dataSize != 2
            || event.first > 127 || event.second > 127
            || (kind != 0x80 && !(kind == 0x90 && event.second == 0))) return std::nullopt;
        auto updated = *this;
        uint16_t matched = 0;
        for (unsigned index = 16; index-- > 0;)
        {
            if (!AcceptNotePart(event.status&15,index,routing[index],mode)) continue;
            const auto selection = SelectNoteRelease(event.first,routing[index].noteFlags);
            if (!selection || selection->path == NoteReleaseSelection::Path::monophonic) return std::nullopt;
            const auto result = updated.noteOff(event,index,selection->selector);
            if (!result) return std::nullopt;
            if (*result) matched |= uint16_t(1u<<index);
        }
        *this = updated;
        return matched;
    }

private:
    std::array<Part,16> parts;
};
}
