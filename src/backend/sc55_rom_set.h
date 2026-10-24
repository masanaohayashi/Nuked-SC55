#pragma once
// The five ROMs of an SC-55 (v1.21): the controller's program ROM and its
// second ROM, whose tables become the native sound data, and the three wave
// ROMs. Files are recognized by content (SHA-256), whatever their names.
//
// The wave ROMs are wired to the PCM chip with their address and data lines
// in a different order; dumps keep the ROM's own order, so loading puts each
// byte back where the chip sees it.

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "sc55_sha256.h"

namespace sc55
{
struct RomSet
{
    std::vector<uint8_t> program;       // R15199778, controller program
    std::vector<uint8_t> programExtra;  // R15209363, controller tables
    std::array<std::vector<uint8_t>, 3> waves; // R15209276/77/81, waves A/B/C, in chip order

    static constexpr std::size_t waveSize = 0x100000;
};

namespace rom
{
enum class Part { program, programExtra, waveA, waveB, waveC };

struct Known { Sha256 digest; Part part; };
inline constexpr std::array<Known, 5> v121{{
    {sha256Hex("7e1bacd1d7c62ed66e465ba05597dcd60dfc13fc23de0287fdbce6cf906c6544"), Part::program},
    {sha256Hex("effc6132d68f7e300aaef915ccdd08aba93606c22d23e580daf9ea6617913af1"), Part::programExtra},
    {sha256Hex("5655509a531804f97ea2d7ef05b8fec20ebf46216b389a84c44169257a4d2007"), Part::waveA},
    {sha256Hex("c655b159792d999b90df9e4fa782cf56411ba1eaa0bb3ac2bdaf09e1391006b1"), Part::waveB},
    {sha256Hex("334b2d16be3c2362210fdbec1c866ad58badeb0f84fd9bf5d0ac599baf077cc2"), Part::waveC},
}};

// Where each ROM line meets the chip: chip address bit n comes from ROM
// address bit addressLine[n], chip data bit n from ROM data bit dataLine[n].
inline constexpr std::array<uint8_t, 20> addressLine{2, 0, 3, 4, 1, 9, 13, 10, 18, 17, 6, 15, 11, 16, 8, 5, 12, 7, 14, 19};
inline constexpr std::array<uint8_t, 8> dataLine{2, 0, 4, 5, 7, 6, 3, 1};

inline std::vector<uint8_t> toChipOrder(std::span<const uint8_t> dump)
{
    std::vector<uint8_t> chip(dump.size());
    for (uint32_t chipAddress = 0; chipAddress < chip.size(); ++chipAddress) {
        uint32_t romAddress = chipAddress & ~0xfffffu;
        for (unsigned bit = 0; bit < addressLine.size(); ++bit)
            romAddress |= ((chipAddress >> bit) & 1u) << addressLine[bit];
        const uint8_t romByte = romAddress < dump.size() ? dump[romAddress] : 0;
        uint8_t chipByte = 0;
        for (unsigned bit = 0; bit < dataLine.size(); ++bit)
            chipByte = uint8_t(chipByte | (((romByte >> dataLine[bit]) & 1u) << bit));
        chip[chipAddress] = chipByte;
    }
    return chip;
}

inline std::optional<std::vector<uint8_t>> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}
}

// Looks through `directory` for the five v1.21 ROMs. On failure, `error`
// says what is missing.
inline std::optional<RomSet> LoadRomSet(const std::filesystem::path& directory, std::string* error = nullptr)
{
    const auto fail = [&](std::string message) -> std::optional<RomSet> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    std::error_code code;
    if (!std::filesystem::is_directory(directory, code)) return fail("Not a folder: " + directory.string());

    std::array<std::optional<std::vector<uint8_t>>, 5> found;
    for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
        if (!entry.is_regular_file(code) || entry.file_size(code) > 4 * RomSet::waveSize) continue;
        auto bytes = rom::readFile(entry.path());
        if (!bytes) continue;
        const auto hash = sha256(*bytes);
        for (const auto& known : rom::v121)
            if (known.digest == hash) found[std::size_t(known.part)] = std::move(*bytes);
    }

    static constexpr const char* names[5]{"program ROM (R15199778)", "second program ROM (R15209363)",
        "wave ROM A (R15209276)", "wave ROM B (R15209277)", "wave ROM C (R15209281)"};
    std::string missing;
    for (std::size_t i = 0; i < found.size(); ++i)
        if (!found[i]) missing += (missing.empty() ? "" : ", ") + std::string(names[i]);
    if (!missing.empty()) return fail("SC-55 v1.21 ROMs not found: " + missing);

    RomSet set;
    set.program = std::move(*found[0]);
    set.programExtra = std::move(*found[1]);
    for (unsigned i = 0; i < 3; ++i) set.waves[i] = rom::toChipOrder(*found[2 + i]);
    return set;
}
}
