#pragma once
#include <array>
#include <cstdint>

namespace sc55
{
// One audio-owner transaction, after a voice's control tick. This updates
// control values without restarting oscillator/filter/envelope history.
struct VoiceRenderUpdate
{
    uint16_t phaseIncrement=0;
    std::array<uint16_t,3> rampCommands{};
    int8_t panLeft=0,panRight=0,reverbSend=0,chorusSend=0;
    uint8_t resonance=0,filterFlags=0;
};
// Prepared oscillator plus initial controls. Key enable remains a separate
// scheduler transition, after all members of a paired note are installed.
struct VoiceRenderStart
{
    uint32_t start=0,loop=0,end=0;
    uint16_t mode=0;
    VoiceRenderUpdate controls;
};
}
