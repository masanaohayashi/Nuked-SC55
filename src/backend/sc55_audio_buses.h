#pragma once
#include <cstdint>
#include <cmath>

namespace sc55
{
struct AudioBuses
{
    int32_t left=0,right=0,reverb=0,chorus=0;
};

// Preserve the existing signal arithmetic: sign-extend each20-bit operand
// before adding. This is wrap/truncation at the next addition, not saturation.
inline int32_t AddBusReturn(int32_t bus,int32_t contribution) noexcept
{
    const auto signed20=[](int32_t value) {
        const auto bits=uint32_t(value)&0xfffff;
        return bits>=0x80000 ? int32_t(bits)-0x100000 : int32_t(bits);
    };
    return signed20(bus)+signed20(contribution>>1)+(contribution&1);
}

// Three stereo return stages, with the same send-feedback routing/order as
// the reference network. No chip state, bus registers or allocation required.
inline AudioBuses MixVoiceAndEffectBuses(const float voices[4],const int dry[6],const int sends[6]) noexcept
{
    AudioBuses result{int32_t(std::lrint(voices[0])),int32_t(std::lrint(voices[1])),
        int32_t(std::lrint(voices[2])),int32_t(std::lrint(voices[3]))};
    for(unsigned stage=0;stage<3;++stage) {
        result.left=AddBusReturn(result.left,dry[stage*2]);
        result.right=AddBusReturn(result.right,dry[stage*2+1]);
    }
    result.chorus=AddBusReturn(result.chorus,sends[0]);
    result.chorus=AddBusReturn(result.chorus,sends[1]);
    result.reverb=AddBusReturn(result.reverb,sends[2]);
    result.chorus=AddBusReturn(result.chorus,sends[3]);
    result.reverb=AddBusReturn(result.reverb,sends[4]);
    result.chorus=AddBusReturn(result.chorus,sends[5]);
    return result;
}
}
