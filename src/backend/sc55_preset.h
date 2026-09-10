#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace sc55
{
// Setup-imported03:0000 table. Values are tone IDs, not emulated pointers.
struct MelodicPresetTable
{
    std::array<uint16_t,128*128> tones{};
    struct Selection { uint8_t bank; uint16_t tone; };
    // MIDI program selection04:09c4..0a24. Preserve both the selected bank
    // and the tone; the requested bank latch is a separate MIDI state.
    std::optional<Selection> resolve(unsigned bank,unsigned program) const noexcept
    {
        if (bank >= 128 || program >= 128) return std::nullopt;
        if (tones[bank*128+program] == 0xffff)
        {
            if (bank >= 64 || program >= 120) return std::nullopt;
            bank &= 0x78;
            if (tones[bank*128+program] == 0xffff) bank = 0;
        }
        const auto tone = tones[bank*128+program];
        if (tone&0x8000) return std::nullopt;
        return Selection{uint8_t(bank),tone};
    }
};

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

// Legacy capital-only entry for callers without imported configuration. Full
// bank selection uses MelodicPresetTable; rhythm has a separate preset owner.
inline std::optional<uint8_t> ResolveV121MelodicPreset(unsigned bankMsb, unsigned program) noexcept
{
    if (bankMsb != 0 || program >= v121CapitalToneIndices.size()) return std::nullopt;
    return v121CapitalToneIndices[program];
}
}
