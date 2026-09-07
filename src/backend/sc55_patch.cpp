#include "sc55_patch.h"
#include "sha256.h"

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
        if (start + (size_t) n * SC55Patch::SIZE + SC55Patch::SIZE > rom.size())
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

    // v1.21 00:1232 / 00:12b3: group 0xffff is the absent-partial sentinel.
    // Other note/velocity-dependent gates are evaluated separately.
    partial.used = block[2] != 0xff || block[3] != 0xff;

    // Legacy dump interpretation only; not the verified runtime envelope map.
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

    // Name scanning cannot distinguish the following multisample name PIANO1
    // from a patch. Apply the verified boundary only to this exact ROM image.
    if (rom.size() == 0x40000)
    {
        SHA256Context context;
        SHA256_Digest digest{};
        if (SHA256Reset(&context) != shaSuccess
            || SHA256Input(&context, rom.data(), unsigned(rom.size())) != shaSuccess
            || SHA256Result(&context, digest.data()) != shaSuccess)
            return false;
        if (digest == SHA256_ToDigest("effc6132d68f7e300aaef915ccdd08aba93606c22d23e580daf9ea6617913af1"))
            return loadRecords(rom, 0x10000, 224);
    }

    if (rom.size() < (size_t) SC55Patch::SIZE * 8)
        return false;

    int best = 0;
    for (uint32_t start = 0; start + SC55Patch::SIZE * 8 <= rom.size(); start += 4)
    {
        const int run = longest_run(rom, start);
        if (run > best) { best = run; table_base = start; }
    }

    if (best < 8)
        return false;

    return loadRecords(rom, table_base, std::min(best, MAX_PATCHES));
}

bool SC55PatchTable::loadRecords(std::span<const uint8_t> rom, uint32_t start, int recordCount)
{
    count = 0;
    table_base = 0;
    if (recordCount <= 0 || recordCount > MAX_PATCHES || start > rom.size()
        || size_t(recordCount) > (rom.size() - start) / SC55Patch::SIZE)
        return false;
    for (int i = 0; i < recordCount; ++i)
        if (!printable_name(rom.subspan(start + size_t(i) * SC55Patch::SIZE,
                                        SC55Patch::NAME_LENGTH)))
            return false;
    table_base = start;
    count = recordCount;

    for (int i = 0; i < count; ++i)
    {
        const uint32_t at = table_base + (uint32_t) i * SC55Patch::SIZE;
        SC55Patch& patch = patches[i];
        patch.rom_offset = at;

        const auto name = rom.subspan(at + SC55Patch::NAME_OFFSET, SC55Patch::NAME_LENGTH);
        patch.name.assign(name.begin(), name.end());
        while (! patch.name.empty() && patch.name.back() == ' ')
            patch.name.pop_back();

        read_partial(patch.partial[0], rom.subspan(at + SC55Patch::PARTIAL_OFFSET, SC55Partial::SIZE));
        read_partial(patch.partial[1], rom.subspan(at + SC55Patch::PARTIAL_OFFSET + SC55Partial::SIZE, SC55Partial::SIZE));

        std::memcpy(patch.common, rom.data() + at + SC55Patch::COMMON_OFFSET,
                    SC55Patch::COMMON_LENGTH);
    }

    return true;
}
