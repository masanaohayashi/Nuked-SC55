#pragma once
#include <array>
#include <cstdint>

namespace sc55
{
struct ModulationDepthTables
{
    std::array<uint16_t,128> secondEnvelope{},pitch{};
    bool operator==(const ModulationDepthTables&) const = default;
};

struct ModulationPreparationTables
{
    std::array<uint16_t,256> timing{};
    ModulationDepthTables depths;
    bool operator==(const ModulationPreparationTables&) const = default;
};
}
