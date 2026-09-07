#pragma once
#include "sc55_voice_lifecycle.h"
#include "sc55_note_dispatch.h"

namespace sc55
{
// 0ca5..0cb6: the positive-candidate rhythm path, after tone/velocity expansion.
// Single-owner, one note per object. All arguments are owned values, not ROM
// pointers. No PCM clock advancement: later installation/startup may wait.
// Retain this result across those waits rather than repeating earlier stops.
class RhythmGroupAdmission
{
public:
    enum class Status { ready, invalidInput, failed, capacityRejected, allocated };

    RhythmGroupAdmission(VoiceAllocator::GroupRequest request,uint8_t partNoteFlags,
        std::array<uint8_t,16> retained,VoiceCapacityPolicy policy) noexcept
        : request_(request), partNoteFlags_(partNoteFlags), retained_(retained), policy_(policy)
    {
        // Capacity1737 reads the same part+5 byte as repeated-note17b8.
        policy_.forceOldest = (partNoteFlags_&16) != 0;
    }

    Status status() const noexcept { return status_; }
    const std::optional<VoiceAllocator::GroupAllocation>& group() const noexcept { return group_; }

    template<class Read,class Write>
    Status run(VoiceAllocator& allocator,std::array<VoiceStopState,24>& lifecycle,Read&& read,Write&& write)
    {
        if (status_ != Status::ready) return status_;
        if (request_.part >= 16 || request_.value > 127 || request_.voiceCount < 1
            || request_.voiceCount > 2 || policy_.startPartControl >= 16 || allocator.freeCount > 24)
            return status_ = Status::invalidInput;
        for (unsigned part = 0; part < 16; ++part)
            if (policy_.reserves[part] > 24 || allocator.partVoiceCount[part] > 24)
                return status_ = Status::invalidInput;
        // Once processing begins a failure is terminal: an earlier phase may
        // have stopped hardware or marked groups. Never silently replay it.
        status_ = Status::failed;
        if (!StopRhythmExclusiveGroups(allocator,lifecycle,request_.part,request_.fieldA3D1,read,write)) return status_;
        if (!RetireRepeatedNote(allocator,lifecycle,request_.part,request_.value,request_.fieldA3D1,
            partNoteFlags_,retained_,read,write)) return status_;
        const auto capacity = EnsureVoiceCapacity(allocator,lifecycle,request_.part,request_.voiceCount,policy_,read,write);
        if (!capacity) return status_;
        // Firmware returns nonzero to0cb6 and abandons this note. Prior stops
        // remain committed; this is not a deferred request to retry later.
        if (!*capacity) return status_ = Status::capacityRejected;
        group_ = allocator.createGroup(request_);
        if (!group_) return status_;
        return status_ = Status::allocated;
    }

private:
    VoiceAllocator::GroupRequest request_;
    uint8_t partNoteFlags_;
    std::array<uint8_t,16> retained_;
    VoiceCapacityPolicy policy_;
    Status status_ = Status::ready;
    std::optional<VoiceAllocator::GroupAllocation> group_;
};

// Prepared rhythm tone -> admitted group -> ordered sample-install input.
// Receive adjustment/mapping/velocity expansion have already run. SoundData
// must remain the same immutable generation through installation and startup.
class RhythmNoteAdmission
{
public:
    enum class Status { ready, invalidInput, absent, velocityRejected, capacityRejected, failed, allocated };
    RhythmNoteAdmission(RhythmNoteVelocity prepared,unsigned part,uint8_t partNoteFlags,
        std::array<uint8_t,16> retained,VoiceCapacityPolicy policy)
        : prepared_(prepared), part_(part), partNoteFlags_(partNoteFlags), retained_(retained), policy_(policy) {}

    Status status() const noexcept { return status_; }
    const std::optional<MelodicAllocationResult>& allocation() const noexcept { return allocation_; }

    template<class Read,class Write>
    Status run(const SoundData& data,VoiceAllocator& allocator,
        std::array<VoiceStopState,24>& lifecycle,Read&& read,Write&& write)
    {
        if (status_ != Status::ready) return status_;
        status_ = Status::invalidInput;
        if (part_ >= 16 || prepared_.mapping.key > 127) return status_;
        if (!prepared_.mapping.present())
            return status_ = prepared_.note ? Status::invalidInput : Status::absent;
        if (!prepared_.note) return status_;
        const auto& note = *prepared_.note;
        const auto* patch = data.patch(note.tone);
        if (!patch || note.tone != prepared_.mapping.tone || note.note != prepared_.mapping.key
            || note.velocity == 0 || note.velocity > 127 || note.partials.candidates.count > 2) return status_;
        if (note.partials.candidates.count == 0) return status_ = Status::velocityRejected;
        RhythmGroupAdmission admission({uint8_t(part_),prepared_.mapping.group,note.note,
            prepared_.mapping.flags,note.partials.candidates.count},partNoteFlags_,retained_,policy_);
        const auto result = admission.run(allocator,lifecycle,read,write);
        using GroupStatus = RhythmGroupAdmission::Status;
        if (result == GroupStatus::invalidInput) return status_;
        if (result == GroupStatus::capacityRejected) return status_ = Status::capacityRejected;
        status_ = Status::failed;
        if (result != GroupStatus::allocated || !admission.group()) return status_;
        const auto group = *admission.group();
        const auto dispatch = PlanPartialVoiceDispatch(*patch,note.partials.candidates.flags,
            {group.voices[0],group.voices[1]});
        if (!dispatch) return status_;
        // Reuse the existing sample-install handoff; its historical name does
        // not imply melodic tone routing. No bank2 ID is narrowed to a byte.
        allocation_ = MelodicAllocationResult{MelodicAllocationResult::Status::allocated,note,group,*dispatch};
        return status_ = Status::allocated;
    }

private:
    RhythmNoteVelocity prepared_;
    unsigned part_;
    uint8_t partNoteFlags_;
    std::array<uint8_t,16> retained_;
    VoiceCapacityPolicy policy_;
    Status status_ = Status::ready;
    std::optional<MelodicAllocationResult> allocation_;
};
}
