#pragma once
#include "sc55_voice_allocator.h"

// Test projection only. Do not reintroduce parallel compatibility arrays into
// the synth just to compare a single allocation field before/after an operation.
inline std::array<uint8_t,24> SnapshotAllocationField(const sc55::VoiceAllocator& allocator,
    uint8_t sc55::VoiceAllocator::VoiceAllocation::*field)
{
    std::array<uint8_t,24> result{};
    for(unsigned i=0;i<result.size();++i) result[i]=allocator.allocations[i].*field;
    return result;
}
