#pragma once
#include "sc55_rhythm.h"

namespace sc55
{
// Owned ROM data, populated at setup, never firmware code or a live RAM view.
// Keep the complete map record: level/pan/sends and name are needed by the
// remaining GS owner even though the note mapper currently uses only four rows.
struct RhythmPresetTable
{
    static std::optional<unsigned> nrpnOutputOffset(uint8_t msb,uint8_t key) noexcept
    {
        if (key>=128) return {};
        switch (msb) {
            case 0x1a: return 0x100+key;
            case 0x1c: return 0x280+key;
            case 0x1d: return 0x300+key;
            case 0x1e: return 0x380+key;
            default: return {};
        }
    }
    using Record = std::array<uint8_t,0x48c>;
    std::array<uint8_t,128> programs{};
    std::array<Record,14> records{};
    std::array<uint8_t,128> program127Accumulators{};

    // NRPN18 uses the committed (fallback-resolved) program latch's ROM
    // baseline, never the edited map. A rejected program leaves it alone.
    bool writeRelativePitch(Record& record,unsigned program,unsigned key,uint8_t value) const noexcept
    {
        if (program>=128 || key>=128 || value>=128) return false;
        const auto index=programs[program];
        if (index&0x80) return true;
        if (index>=records.size()) return false;
        const int pitch=int(records[index][0x180+key])+value-64;
        record[0x180+key]=uint8_t(pitch<0 ? 0 : pitch>127 ? 127 : pitch);
        return true;
    }

    // 04:0964..0998, MIDI program-change policy. Unassigned programs >=64
    // reject the tone and do not replace the existing shared map. Below64,
    // first try the eight-program family, then program0.
    std::optional<uint8_t> resolve(unsigned program) const noexcept
    {
        if (program >= 128) return std::nullopt;
        if (programs[program] == 255)
        {
            if (program >= 64) return std::nullopt;
            program &= 0x78;
            if (programs[program] == 255) program = 0;
        }
        return programs[program] < records.size() ? std::optional<uint8_t>(uint8_t(program)) : std::nullopt;
    }

    static RhythmKeyMap decode(const Record& record) noexcept
    {
        RhythmKeyMap result;
        for (unsigned key = 0; key < 128; ++key)
        {
            result.tones[key] = uint16_t((record[2*key]<<8)|record[2*key+1]);
            result.pitches[key] = record[0x180+key];
            result.groups[key] = record[0x200+key];
            result.flags[key] = record[0x400+key];
        }
        return result;
    }

    enum class WriteResult { applied, unsupported, invalidLength };
    struct BulkWriteResult { WriteResult status; unsigned map=0; };
    //49 mb 00: 1680..16f2 remaps the first four64-byte blocks. Unlike
    //ordinary GS fields, these are raw bytes and use addition, not nibble OR.
    static BulkWriteResult writeBulk(std::array<Record,2>& maps,
        std::span<const uint8_t> payload) noexcept
    {
        if(payload.size()<3) return {WriteResult::invalidLength};
        if(payload[0]!=0x49 || payload[1]>=0x20 || payload[2]!=0)
            return {WriteResult::unsupported};
        const unsigned map=payload[1]>>4, block=payload[1]&15;
        //Bulk starts at8848; the owned map starts at8748. Its first256
        //bytes (tone identifiers) are not part of this transfer region.
        const unsigned offset=0x100+(block<4 ? (block^2) : block)*64;
        const auto bytes=payload.subspan(3);
        const auto count=std::max<std::size_t>(1,(bytes.size()+1)/2);
        if(offset>=maps[map].size() || count>maps[map].size()-offset)
            return {WriteResult::invalidLength};
        //EE9 returnsff at end of input. The ROM still performs one write
        //for an empty block and pads an unmatched high byte withff.
        for(std::size_t i=0;i<count;++i) {
            const auto high=2*i<bytes.size() ? bytes[2*i] : uint8_t(255);
            const auto low=2*i+1<bytes.size() ? bytes[2*i+1] : uint8_t(255);
            maps[map][offset+i]=uint8_t((high<<4)+low);
        }
        return {WriteResult::applied,map};
    }
    // GS41 mx kk, table03:dc16 / handler04:101d. Byte rows consume the
    // transaction's data in address order; flag rows consume only the first
    // value. This is not continuation into the next GS parameter-table row.
    static WriteResult write(Record& record,unsigned field,unsigned key,
        std::span<const uint8_t> values) noexcept
    {
        if (field > 8 || key > 127 || (field == 0 && key != 0)) return WriteResult::unsupported;
        if (values.empty()) return WriteResult::invalidLength;
        if (field == 7 || field == 8)
        {
            const uint8_t mask = field == 7 ? 1 : 16;
            auto& flags = record[0x400+key];
            flags = uint8_t((flags&~mask) | (values[0] ? mask : 0));
            return WriteResult::applied;
        }
        constexpr std::array<unsigned,7> offsets{0x480,0x180,0x100,0x200,0x280,0x300,0x380};
        const auto offset = offsets[field]+key;
        if ((field == 0 && values.size() != 12) || values.size() > record.size()-offset)
            return WriteResult::invalidLength;
        for (unsigned i = 0; i < values.size(); ++i)
            record[offset+i] = field == 0 && values[i] < 32 ? 32 : values[i] > 127 ? 127 : values[i];
        return WriteResult::applied;
    }
};
}
