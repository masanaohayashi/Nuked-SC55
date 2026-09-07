#pragma once
#include <array>
#include <cstdint>

namespace sc55
{
// Two firmware link tables, CAC4/CADC. Their role in allocation is still
// separate from this verified detach operation; do not infer a linked list.
struct VoiceLinks
{
    static constexpr unsigned count = 24;
    static constexpr uint8_t absent = 0xff;
    std::array<uint8_t,count> first, second;
    VoiceLinks() noexcept { first.fill(absent); second.fill(absent); }

    // v1.21 339b..33c9. First link has priority when both are present.
    // Detach clears both fields of this voice and the selected partner;
    // it does not chase another link or terminate the partner's envelope.
    bool detach(unsigned voice) noexcept
    {
        if (voice >= count) return false;
        const auto partner = first[voice] != absent ? first[voice] : second[voice];
        if (partner == absent) return true;
        if (partner >= count) return false; // no partial mutation on bad data
        first[voice] = second[voice] = absent;
        first[partner] = second[partner] = absent;
        return true;
    }
};

// Allocator-side links (A390/A3A8) and note-group endpoints (A2A0/A2B8).
// Separate from VoiceLinks: firmware updates these in 1e7e..1ecd, after
// envelope termination has already cleared the corresponding PCM links.
struct VoiceGroupLinks
{
    std::array<uint8_t,24> next, previous, head, tail;
    VoiceGroupLinks() noexcept
    { next.fill(0xff); previous.fill(0xff); head.fill(0xff); tail.fill(0xff); }

    bool detach(unsigned voice, unsigned group, VoiceLinks& pcmLinks) noexcept
    {
        if (voice >= 24 || group >= 24) return false;
        const auto successor = next[voice];
        const auto predecessor = previous[voice];
        // Firmware uses the sign bit, not equality with ff, as sentinel.
        if (successor < 128)
        {
            if (successor >= 24) return false;
            head[group] = successor;
            previous[successor] = 0xff;
            pcmLinks.first[successor] = 0xff;
            next[voice] = 0xff;
            pcmLinks.second[voice] = 0xff;
        }
        else
        {
            if (predecessor < 128)
            {
                if (predecessor >= 24) return false;
                previous[voice] = 0xff;
                pcmLinks.first[voice] = 0xff;
                next[predecessor] = 0xff;
                pcmLinks.second[predecessor] = 0xff;
            }
            else head[group] = 0xff;
            tail[group] = predecessor;
        }
        return true;
    }
};
}
