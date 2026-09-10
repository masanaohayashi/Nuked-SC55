#include "sc55_voice_allocator.h"
#include "sc55_voice_set.h"
#include <cstdlib>
#include <iostream>
#include <source_location>

static void check(bool value, std::source_location where = std::source_location::current())
{ if (!value) { std::cerr << "Failed line " << where.line() << '\n'; std::abort(); } }

int main()
{
    for (unsigned limit = 24; limit <= 128; limit += 4)
    {
        std::cout << "limit=" << limit << std::endl;
        sc55::BasicVoiceAllocator<128> allocator;
        check(allocator.initializeTables(limit, limit));
        check(allocator.freeCount == limit);
        sc55::VoiceSet allocated;
        for (unsigned i = 0; i < limit; ++i)
        {
            const auto group = allocator.createGroup({uint8_t(i % 16), 0x80, uint8_t(i), 1, 1});
            check(group.has_value());
            const auto voice = group->voices[0];
            check(voice < limit && !allocated.contains(voice));
            allocated.set(voice);
        }
        check(allocator.freeCount == 0 && allocated.count() == limit);
        check(!allocator.createGroup({0, 0x80, 60, 1, 1}));
        for (unsigned i = 0; i < limit; ++i)
            check(allocator.reclaimStoppedVoice(i, allocator.allocations[i].noteGroup,
                                                allocator.allocations[i].part, false));
        check(allocator.freeCount == limit);
        for (const auto count : allocator.partVoiceCount) check(count == 0);
        // Paired notes must consume two physical voices, not one note counter.
        for (unsigned i = 0; i < limit / 2; ++i)
            check(allocator.createGroup({0, 0x80, uint8_t(i), 1, 2}).has_value());
        check(allocator.freeCount == 0 && allocator.partVoiceCount[0] == limit);
        for (unsigned i = 0; i < limit; ++i)
            check(allocator.reclaimStoppedVoice(i, allocator.allocations[i].noteGroup,
                                                allocator.allocations[i].part, false));
        check(allocator.freeCount == limit && allocator.partVoiceCount[0] == 0);
    }
    sc55::VoiceAllocator original;
    check(original.initializeTables());
    check(original.freeCount == 24 && original.allocations.size() == 24);
    std::cout << "Voice allocation 24..128 step4: single/paired exhaustion and reclaim PASS\n";
}
