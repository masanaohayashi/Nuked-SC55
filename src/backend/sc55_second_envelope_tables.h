#pragma once
#include <array>
#include <cstdint>

namespace sc55
{
using SecondEnvelopeKeyTables = std::array<std::array<uint16_t,256>,16>;
struct SecondEnvelopeTimingKeyCurves
{
    std::array<std::array<uint8_t,256>,16> attack{}, release{};
    bool operator==(const SecondEnvelopeTimingKeyCurves&) const = default;
};

struct SecondEnvelopeTargetTables
{
    std::array<uint16_t,192> startSensitivity{}; // 74d2
    std::array<uint16_t,192> velocitySensitivity{}; // 74fc
    std::array<uint16_t,256> scale{}; // 7512
    bool operator==(const SecondEnvelopeTargetTables&) const = default;
};

// Owned lookup data for 46e0..478e. Loading/export belongs outside rendering.
struct SecondEnvelopePreparationTables
{
    SecondEnvelopeKeyTables keys{};
    SecondEnvelopeTargetTables targets;
    SecondEnvelopeTimingKeyCurves timing;
    bool operator==(const SecondEnvelopePreparationTables&) const = default;
};

struct SecondEnvelopePcmTables
{
    std::array<uint8_t,256> smoothingCeiling{}; // 7714
    std::array<uint16_t,129> levelCurve{}; // 7612, includes interpolation successor
    std::array<uint8_t,128> outputCeiling{}; // 7816
    bool operator==(const SecondEnvelopePcmTables&) const = default;
};
}
