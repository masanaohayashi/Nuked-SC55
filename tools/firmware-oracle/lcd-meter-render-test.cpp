#include "sc55_lcd_meter_render.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "LCD meter render: %s\n", message);
        std::exit(1);
    }
}
}

int main()
{
    constexpr std::size_t width = 741;
    constexpr std::size_t height = 268;
    std::vector<uint8_t> pixels(width * height, 0);
    std::array<unsigned, 16> bars{};
    bars[0] = 1;
    bars[1] = 16;
    bars[15] = 9;

    sc55::renderNativeLevelMeterPixels(pixels, width, bars);
    const auto pixel = [&](unsigned x, unsigned y) { return pixels[y * width + x]; };

    // Part 1: one bottom cell only, with the cell gap left untouched.
    require(pixel(293, 236) == 1, "part 1's lowest bar cell is missing or shifted");
    require(pixel(293, 225) == 2, "part 1 has a lit cell above its level");
    require(pixel(317, 236) == 0, "part 1's bar spills into the next column");

    // Part 2: full scale spans both 8-row matrices in one aligned column.
    require(pixel(319, 71) == 1 && pixel(319, 236) == 1,
            "part 2's full-scale cells do not span the two meter matrices");
    require(pixel(319, 70) == 0 && pixel(318, 71) == 0,
            "part 2's bar escaped its cell bounds");

    // Part 16 is the single-column fourth CG group, still on column 16.
    require(pixel(683, 148) == 1 && pixel(683, 159) == 1,
            "part 16 was not drawn in the last meter column");
    require(pixel(683, 137) == 2 && pixel(707, 159) == 0,
            "part 16's level or right edge is misaligned");

    // Captured from the unmodified v1.21 H8 interpreter + LCD_Render.
    // This fixture is deliberately independent of the renderer's indexing.
    // Regenerate/check with tools/lcd-meter-oracle (local ROM required).
    constexpr std::array<uint8_t, 64> cgram {
        0x00,0x25,0x4a,0x6f,0x14,0x39,0x5e,0x03,
        0x2b,0x50,0x75,0x1a,0x3f,0x64,0x09,0x2e,
        0x56,0x7b,0x20,0x45,0x6a,0x0f,0x34,0x59,
        0x01,0x26,0x4b,0x70,0x15,0x3a,0x5f,0x04,
        0x2c,0x51,0x76,0x1b,0x40,0x65,0x0a,0x2f,
        0x57,0x7c,0x21,0x46,0x6b,0x10,0x35,0x5a,
        0x02,0x27,0x4c,0x71,0x16,0x3b,0x60,0x05,
        0x2d,0x52,0x77,0x1c,0x41,0x66,0x0b,0x30
    };
    // Top to bottom; bit 0 is part 1, bit 15 is part 16.
    constexpr std::array<uint16_t, 16> romRows {
        0x19a0,0x4774,0x340a,0xee9e,0x8145,0xd3d3,0x28af,0x7a78,
        0x761a,0x9d81,0xc355,0xb02b,0x6abf,0x0564,0x57f2,0xac8e
    };
    std::fill(pixels.begin(), pixels.end(), 0);
    require(sc55::renderSysExLevelMeterPixels(pixels, width, cgram),
            "valid 64-byte meter CGRAM was rejected");
    for (unsigned row = 0; row < 16; ++row)
        for (unsigned part = 0; part < 16; ++part)
        {
            const auto expected = (romRows[row] & (1u << part)) != 0 ? 1 : 2;
            const auto x = 293u + part * 26u;
            const auto y = 71u + row * 11u;
            for (unsigned dy = 0; dy < 9; ++dy)
                for (unsigned dx = 0; dx < 24; ++dx)
                    require(pixel(x + dx, y + dy) == expected,
                            "SysEx meter differs from the original ROM LCD output");
            require(pixel(x + 24, y) == 0 && pixel(x, y + 9) == 0,
                    "SysEx cell spills into a grid gap");
        }

    std::puts("LCD meter render: native levels and SysEx CGRAM alignment PASS");
}
