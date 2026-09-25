#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sc55
{
// Model45 sends eight 5x8 CGRAM glyphs, ordered as upper/lower pairs for
// each five-part group. The v1.21 ROM writes DDRAM 20..23 = {0,2,4,6}
// and 60..63 = {1,3,5,7}; its 04:30b9 transfer preserves CGRAM byte order.
// Glyphs 6/7 contribute only their first column (part 16).
// Verified against actual H8 MIDI reception and LCD_Render by lcd-meter-oracle.
inline bool renderSysExLevelMeterPixels(std::span<uint8_t> pixels,
                                        std::size_t stride,
                                        const std::array<uint8_t, 64>& cgram) noexcept
{
    constexpr std::size_t width = 741;
    constexpr std::size_t height = 268;
    if (stride < width || pixels.size() < (height - 1) * stride + width)
        return false;

    for (unsigned matrix = 0; matrix < 2; ++matrix)
    {
        const auto matrixTop = 71u + matrix * 88u;
        for (unsigned part = 0; part < 16; ++part)
        {
            const auto screenX = 293u + part * 26u;
            const auto glyph = (part / 5) * 2 + matrix;
            const auto glyphColumn = part % 5;
            const auto bit = uint8_t(1u << (4 - glyphColumn));
            for (unsigned row = 0; row < 8; ++row)
            {
                const auto colour = (cgram[glyph * 8 + row] & bit) != 0
                                  ? uint8_t(1) : uint8_t(2);
                const auto screenY = matrixTop + row * 11u;
                for (unsigned y = 0; y < 9; ++y)
                    std::fill_n(pixels.data() + (screenY + y) * stride + screenX,
                                24, colour);
            }
        }
    }
    return true;
}

// The native renderer owns synthetic meter levels rather than the firmware's
// CGRAM when no meter bitmap SysEx is active. Draw its 16x16 cells directly.
inline bool renderNativeLevelMeterPixels(std::span<uint8_t> pixels,
                                         std::size_t stride,
                                         const std::array<unsigned, 16>& bars) noexcept
{
    constexpr std::size_t width = 741;
    constexpr std::size_t height = 268;
    if (stride < width || pixels.size() < (height - 1) * stride + width)
        return false;

    for (unsigned part = 0; part < bars.size(); ++part)
    {
        const auto screenX = 293u + part * 26u;
        for (unsigned matrix = 0; matrix < 2; ++matrix)
        {
            const auto matrixTop = 71u + matrix * 88u;
            for (unsigned row = 0; row < 8; ++row)
            {
                const auto threshold = matrix == 0 ? 16u - row : 8u - row;
                const auto colour = bars[part] >= threshold ? uint8_t(1) : uint8_t(2);
                const auto screenY = matrixTop + row * 11u;
                for (unsigned y = 0; y < 9; ++y)
                    std::fill_n(pixels.data() + (screenY + y) * stride + screenX,
                                24, colour);
            }
        }
    }
    return true;
}
}
