#pragma once
#include "sc55_envelope_setup.h"
#include <stdexcept>

inline sc55::EnvelopeKeyLevelTables importEnvelopeKeyLevels(std::span<const uint8_t> rom1,std::span<const uint8_t> rom2)
{
    if (rom1.size() != 32768 || rom2.size() != 262144) throw std::runtime_error("Unexpected key-level ROM size");
    sc55::EnvelopeKeyLevelTables result;
    std::copy_n(rom1.begin()+0x69c6,129,result.adjustment.begin());
    for (unsigned curve = 0; curve < 16; ++curve)
    {
        const unsigned at = 0x3dc72+curve*2;
        const uint16_t pointer = uint16_t((rom2[at]<<8)|rom2[at+1]);
        for (unsigned key = 0; key < 256; ++key) result.curves[curve][key] = rom2[0x30000|uint16_t(pointer+key)];
    }
    return result;
}

// Offline v1.21 importer only. Never called on the native execution path.
inline sc55::EnvelopeKeyTables importEnvelopeKeys(std::span<const uint8_t> rom1,std::span<const uint8_t> rom2)
{
    if (rom1.size() != 32768 || rom2.size() != 262144) throw std::runtime_error("Unexpected key-table ROM size");
    sc55::EnvelopeKeyTables result;
    for (unsigned i = 0; i < 256; ++i)
        result.multipliers[i] = uint16_t((rom1[0x67c6+2*i]<<8)|rom1[0x67c7+2*i]);
    for (unsigned curve = 0; curve < 16; ++curve)
        for (unsigned kind = 0; kind < 2; ++kind)
        {
            const unsigned at = 0x3dc92+kind*32+curve*2;
            const uint16_t pointer = uint16_t((rom2[at]<<8)|rom2[at+1]);
            auto& output = kind == 0 ? result.attack[curve] : result.release[curve];
            for (unsigned key = 0; key < 256; ++key) output[key] = rom2[0x30000|uint16_t(pointer+key)];
        }
    return result;
}
