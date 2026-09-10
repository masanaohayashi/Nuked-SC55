#pragma once
#include <array>
#include <cstdint>

namespace sc55
{
// Resolved reverb/delay network. Coefficients retain SC-55's signed packed
// representation until the signal owner decodes them; no device addresses.
struct ReverbSetup
{
    std::array<uint16_t,12> diffusionTaps{};
    std::array<uint16_t,9> tailTaps{};
    std::array<uint16_t,2> diffusion{},damping{};
    uint16_t comb=0,output=0,spreadCommand=0;
    bool delayProgram=false;
};
}
