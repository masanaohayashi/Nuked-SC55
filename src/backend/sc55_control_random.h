#pragma once
#include <cstdint>

namespace sc55
{
// Sample the shared audio-clocked source. Reading does not advance it; native
// modulation/pan/pitch must not grow independent per-voice random sequences.
template<class Read,class Write>
uint16_t ReadControlRandom(Read& read,Write& write) noexcept
{
    if constexpr(requires { write.randomWord(); }) return write.randomWord();
    else {
        write(uint8_t(0x3e),uint8_t(30)); (void)read(uint8_t(0x34));
        const auto high=read(uint8_t(0x3a)); const auto low=read(uint8_t(0x3b));
        return uint16_t((uint16_t(high)<<8)|low);
    }
}
}
