#include "sc55_patch.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <fstream>
#include <iterator>

int main(int argc,char** argv)
{
    // Construct name-first records independently of parser offset constants.
    std::vector<uint8_t> rom(9 * 216, 0);
    for (int i = 0; i < 9; ++i)
    {
        const size_t at = i * 216;
        std::fill_n(rom.begin() + at, 12, ' ');
        rom[at] = 'A' + i;
        rom[at + 12] = 40 + i;
        rom[at + 32] = 60 + i;
        rom[at + 124] = 80 + i;
    }
    const auto require = [](bool ok) { if (!ok) throw std::runtime_error("patch layout regression"); };
    SC55PatchTable table;
    require(table.load(rom) && table.base() == 0 && table.size() == 9);
    for (int i = 0; i < 9; ++i)
    {
        require(table[i].name == std::string(1, 'A' + i));
        require(table[i].common[0] == 40 + i);
        require(table[i].partial[0].raw[0] == 60 + i);
        require(table[i].partial[1].raw[0] == 80 + i);
    }
    // Nonzero surrounding data must not hide the absent-group sentinel.
    rom[32 + 2] = rom[32 + 3] = 0xff;
    // Conversely, an all-zero partial refers to group zero, not absence.
    std::fill_n(rom.begin() + 124, 92, 0);
    require(table.load(rom));
    require(!table[0].partial[0].used && table[0].partial[1].used);
    // A complete name is insufficient: the partials must also fit.
    rom.resize(8 * 216 + 12);
    require(table.load(rom) && table.size() == 8);
    rom.resize(8 * 216);
    require(table.load(rom) && table.size() == 8);
    rom.resize(8 * 216 - 1);
    require(!table.load(rom) && table.size() == 0);
    // A following sample name is printable too; explicit boundaries must win.
    std::vector<uint8_t> asset(225 * 216, 0);
    for (unsigned i = 0; i < 225; ++i)
        std::fill_n(asset.begin() + i * 216, 12, 'A');
    require(table.loadRecords(asset, 0, 224) && table.size() == 224);
    require(table[223].rom_offset == 223 * 216);
    // Imported data must survive mutation/destruction of the source asset.
    asset[0] = 'Z';
    asset[32] = 91;
    require(table[0].name == std::string(12, 'A'));
    require(table[0].partial[0].raw[0] == 0);
    require(!table.loadRecords(asset, 0, 257) && table.size() == 0);
    require(!table.loadRecords(asset, 0xffffffffu, 224));
    require(!table.loadRecords(asset, 0, -1));
    require(!table.loadRecords(std::span(asset).first(224*216-1), 0, 224));
    asset[223*216] = 0;
    require(!table.loadRecords(asset, 0, 224) && table.size() == 0);
    std::puts("patch layout and truncated-record checks passed");
    if (argc == 2)
    {
        // Optional local extracted bank2 fixture. No control-ROM loader or MCU
        // is linked into this executable; only the data records are opened.
        std::ifstream input(argv[1],std::ios::binary);
        require(bool(input));
        std::vector<uint8_t> records((std::istreambuf_iterator<char>(input)),{});
        require(records.size() == 162*216 && table.loadRecords(records,0,162));
        require(table.size() == 162 && table[0].name == "Glasses" && table[161].name == "Open Hi Hat2");
        for (unsigned p = 0; p < 162; ++p)
        {
            require(table[p].rom_offset == p*216);
            for (unsigned i = 0; i < 20; ++i) require(table[p].common[i] == records[p*216+12+i]);
            for (unsigned partial = 0; partial < 2; ++partial)
                for (unsigned i = 0; i < 92; ++i)
                    require(table[p].partial[partial].raw[i] == records[p*216+32+partial*92+i]);
        }
        records.clear(); records.shrink_to_fit();
        require(table[161].name == "Open Hi Hat2");
        std::puts("Extracted bank2: all162 owned patch records loaded without control ROM");
    }
    else require(argc == 1);
}
