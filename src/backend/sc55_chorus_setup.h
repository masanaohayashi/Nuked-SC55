#pragma once
#include <array>
#include <cstdint>

namespace sc55
{
// Prepared delay-head geometry and routing. All values are derived from the
// SC-55 parameter tables before submission; no register addresses or polling.
struct ChorusSetup
{
    uint32_t begin=0,end=0,position=0;
    uint16_t increment=0;
    std::array<uint16_t,4> returns{};
};
}
