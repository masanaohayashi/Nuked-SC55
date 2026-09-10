#pragma once
#include <cstdint>

namespace sc55
{
// The chorus sweeps two complementary delay read heads. Integer position and
// fractional phase are retained across blocks and silence. This is signal
// state, with no register file, interrupts or CPU execution.
struct ChorusOscillator
{
    uint32_t position=0,begin=0,end=0;
    uint16_t phase=0,increment=0;
    bool descending=false,pingPong=true,reverse=false;

    void advance(bool update=true,bool active=true) noexcept
    {
        const unsigned nextPhase=unsigned(phase)+increment;
        if(!update) return;
        phase=uint16_t(nextPhase&0x3fff);
        if(nextPhase<0x4000) return;

        const bool atEnd=(position&0xfffff)==((descending?begin:end)&0xfffff);
        uint32_t next=position;
        if(atEnd) {
            if(!pingPong) next=begin;
        } else {
            const bool backward=reverse^(pingPong && descending);
            next=uint32_t(int64_t(position)+(backward?-1:1));
        }
        // One integer step per frame, as the existing SC-55 address generator
        // does even if an unusually large increment carries more than once.
        if(active) position=next&0xfffff;
        descending=pingPong && (descending^atEnd);
    }

    uint16_t leftTap() const noexcept { return uint16_t(end-(position-begin)); }
    uint16_t rightTap() const noexcept { return uint16_t(position); }
};
}
