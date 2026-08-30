// 音色テーブルの読み取りが正しいかを確かめる。
//   sc55dumppatch sc55_waverom2.bin [音色番号]
#include "sc55_patch.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("sc55dumppatch <sc55_rom2.bin> [番号]\n"); return 1; }

    std::FILE* file = std::fopen(argv[1], "rb");
    if (! file) { std::fprintf(stderr, "開けません: %s\n", argv[1]); return 1; }
    std::fseek(file, 0, SEEK_END);
    std::vector<uint8_t> rom((size_t) std::ftell(file));
    std::rewind(file);
    if (std::fread(rom.data(), 1, rom.size(), file) != rom.size()) { std::fclose(file); return 1; }
    std::fclose(file);

    SC55PatchTable table;
    if (! table.load(rom)) { std::fprintf(stderr, "音色テーブルが見つかりません\n"); return 1; }

    std::printf("rom[%05x] から %d 音色\n\n", table.base(), table.size());

    if (argc > 2)
    {
        const int index = std::atoi(argv[2]);
        if (index < 0 || index >= table.size()) return 1;
        const SC55Patch& patch = table[index];
        std::printf("%3d  %-12s  rom[%05x]\n", index, patch.name.c_str(), patch.rom_offset);
        for (int p = 0; p < 2; ++p)
        {
            const SC55Partial& partial = patch.partial[p];
            std::printf("  パーシャル %d: %s\n", p + 1, partial.used ? "使用" : "未使用");
            if (! partial.used) continue;
            static const char* label[3] = {"env0 (+12)", "env1 (+4a)", "env2 (+4f)"};
            for (int e = 0; e < 3; ++e)
            {
                std::printf("    %s:", label[e]);
                for (int i = 0; i < 5; ++i) std::printf(" %02x", partial.envelope[e][i]);
                std::printf("   flags=%02x\n", partial.envelope_flags[e]);
            }
        }
        return 0;
    }

    for (int i = 0; i < table.size(); ++i)
        std::printf("%3d  %-12s  %s\n", i, table[i].name.c_str(),
                    table[i].partial[1].used ? "2 パーシャル" : "1 パーシャル");
    return 0;
}
