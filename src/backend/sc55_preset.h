#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace sc55
{
// Indices into the corrected, name-first v1.21 patch table. Established by
// observing the firmware's common-patch pointer during note expansion for all
// 128 programs at bank MSB 0. These are not waveform addresses or voice presets.
// The native runtime can use this mapping without reading firmware instructions.
inline constexpr std::array<uint8_t,128> v121CapitalToneIndices {
    0, 1, 2, 3, 4, 6, 8, 10, 11, 12, 13, 14, 15, 16, 17, 19,
    20, 22, 24, 25, 27, 28, 30, 31, 32, 34, 37, 39, 41, 43, 44, 46,
    48, 49, 50, 51, 52, 53, 54, 56, 58, 59, 60, 61, 62, 63, 64, 65,
    66, 68, 69, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 83, 85,
    87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102,
    103, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
    120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 133, 134, 135, 136,
    137, 138, 139, 140, 142, 144, 146, 148, 149, 152, 154, 160, 163, 169, 179, 185
};

// Unsupported selections stay explicit instead of silently using the wrong
// instrument. Bank fallback rules and rhythm sets need their own implementation.
inline std::optional<uint8_t> ResolveV121MelodicPreset(unsigned bankMsb, unsigned program) noexcept
{
    if (bankMsb != 0 || program >= v121CapitalToneIndices.size()) return std::nullopt;
    return v121CapitalToneIndices[program];
}
}
