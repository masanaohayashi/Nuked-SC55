#include "sc55_patch.h"

#include <algorithm>
#include <cstring>

namespace
{
bool printable_name(std::span<const uint8_t> bytes)
{
    return std::all_of(bytes.begin(), bytes.end(),
                       [] (uint8_t c) { return c >= 32 && c < 127; });
}

// 音色名が正しい間隔で連続する場所を探す。決め打ちのアドレスより ROM の版に強い。
int longest_run(std::span<const uint8_t> rom, uint32_t start)
{
    int n = 0;
    for (;;)
    {
        const uint32_t at = start + (uint32_t) n * SC55Patch::SIZE + SC55Patch::NAME_OFFSET;
        if (at + SC55Patch::NAME_LENGTH > rom.size())
            break;
        if (! printable_name(rom.subspan(at, SC55Patch::NAME_LENGTH)))
            break;
        ++n;
    }
    return n;
}

void read_partial(SC55Partial& partial, std::span<const uint8_t> block)
{
    std::memcpy(partial.raw, block.data(), SC55Partial::SIZE);

    // 未使用のパーシャルは先頭が 00 と ff だけで埋まる。
    partial.used = std::any_of(block.begin(), block.begin() + 8,
                               [] (uint8_t b) { return b != 0x00 && b != 0xff; });

    // 5 セグメントのエンベロープが 3 組。各バイトは下位 7 ビットが値、bit7 がフラグ。
    static constexpr int GROUP[3] = {0x12, 0x4a, 0x4f};
    for (int e = 0; e < 3; ++e)
    {
        uint8_t flags = 0;
        for (int i = 0; i < 5; ++i)
        {
            const uint8_t byte = block[GROUP[e] + i];
            partial.envelope[e][i] = byte & 0x7f;
            flags |= (uint8_t) ((byte >> 7) << i);
        }
        partial.envelope_flags[e] = flags;
    }
}
}

bool SC55PatchTable::load(std::span<const uint8_t> rom)
{
    count = 0;
    table_base = 0;

    if (rom.size() < (size_t) SC55Patch::SIZE * 8)
        return false;

    int best = 0;
    for (uint32_t start = 0; start + SC55Patch::SIZE * 8 < rom.size(); start += 4)
    {
        const int run = longest_run(rom, start);
        if (run > best) { best = run; table_base = start; }
    }

    if (best < 8)
        return false;

    count = std::min(best, MAX_PATCHES);

    for (int i = 0; i < count; ++i)
    {
        const uint32_t at = table_base + (uint32_t) i * SC55Patch::SIZE;
        SC55Patch& patch = patches[i];
        patch.rom_offset = at;

        const auto name = rom.subspan(at + SC55Patch::NAME_OFFSET, SC55Patch::NAME_LENGTH);
        patch.name.assign(name.begin(), name.end());
        while (! patch.name.empty() && patch.name.back() == ' ')
            patch.name.pop_back();

        read_partial(patch.partial[0], rom.subspan(at, SC55Partial::SIZE));
        read_partial(patch.partial[1], rom.subspan(at + SC55Partial::SIZE, SC55Partial::SIZE));

        std::memcpy(patch.common, rom.data() + at + SC55Patch::COMMON_OFFSET,
                    SC55Patch::COMMON_LENGTH);
    }

    return true;
}
