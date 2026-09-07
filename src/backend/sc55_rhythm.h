#pragma once
#include "sc55_voice_setup.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace sc55
{
// Mutable per-key configuration, not a ROM preset table or GS reset image.
// Firmware map0 starts8748, map1 starts8bd4; +000 is128 big-endian
// tone words, +180 the pitch byte, +200 the grouping byte, +400 the flags byte.
struct RhythmKeyMap
{
    std::array<uint16_t,128> tones = [] { std::array<uint16_t,128> a; a.fill(0xffff); return a; }();
    std::array<uint8_t,128> groups{}, flags{};
    std::array<uint8_t,128> pitches{}; // Explicit configuration, not GS defaults.
};

struct RhythmInitialKeys
{
    uint8_t initialKey, sourceKey;
};

// 11fe..1215: use the selected map's per-key pitch, apply part transpose,
// and store the SAME result in A1B3 and A1B4. Unlike melodic11d0, there is
// no master transpose, and sourceKey is not the original incoming key.
inline std::optional<RhythmInitialKeys> PrepareRhythmInitialKeys(unsigned key,
    const RhythmKeyMap& map,uint8_t partShift,uint8_t partOffset) noexcept
{
    if (key >= 128) return std::nullopt;
    const auto pitch = TransposePartKey(map.pitches[key],partShift,partOffset);
    return RhythmInitialKeys{pitch,pitch};
}

struct RhythmToneSelection
{
    uint8_t key, flags, group, velocityAccumulator;
    uint16_t tone; // Preserve negative sentinel; flags/group still update.
    bool present() const noexcept { return (tone&0x8000) == 0; }
    // IDs, not source addresses. Bank2 tone0 follows bank1 tone223.
    uint8_t bank() const noexcept { return tone < 224 ? 1 : 2; }
    uint16_t bankTone() const noexcept { return tone < 224 ? tone : uint16_t(tone-224); }
};

// 0c3c..0c9f/0ccc, before patch-velocity evaluation and exclusive-group
// release/allocation delegates. Caller already selected the rhythm map from
// part+5. Program127 uses the owned key table formerly at03:d168; other
// programs preserve the incoming accumulator (note entry normally clears it).
// A high-bit tone is absent, not a fallback to another instrument. bank()
// and bankTone() are meaningful only when present(). No data/PCM access.
inline std::optional<RhythmToneSelection> SelectRhythmTone(unsigned key,unsigned program,
    const RhythmKeyMap& map,std::span<const uint8_t,128> program127Accumulators,
    uint8_t accumulator) noexcept
{
    if (key >= 128 || program >= 128) return std::nullopt;
    return RhythmToneSelection{uint8_t(key),map.flags[key],map.groups[key],
        program == 127 ? program127Accumulators[key] : accumulator,map.tones[key]};
}
}
