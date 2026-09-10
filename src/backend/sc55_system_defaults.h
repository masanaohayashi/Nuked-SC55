#pragma once
#include "sc55_part_settings.h"
#include "sc55_note_start.h"
#include "sc55_sysex.h"

namespace sc55
{
// Configuration fields not consumed by the native voice/controller models.
// Preserve them for transfer roundtrips instead of keeping a second copy of
// all live settings. These offsets describe the wire format, not CPU memory.
struct UninterpretedSystemSettings
{
    static constexpr std::array<unsigned,17> globalOffsets{
        7,0x29,0x31,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47};
    static constexpr std::array<unsigned,8> partOffsets{0x18,0x19,0x2b,0x37,0x43,0x4f,0x5b,0x67};
    std::array<uint8_t,17> global{};
    std::array<std::array<uint8_t,8>,16> parts{};

    void reset(std::span<const uint8_t,0x748> defaults) noexcept
    {
        for(unsigned i=0;i<global.size();++i) global[i]=defaults[globalOffsets[i]];
        for(unsigned part=0;part<16;++part)
            for(unsigned i=0;i<partOffsets.size();++i) parts[part][i]=defaults[0x48+part*0x70+partOffsets[i]];
    }
    std::optional<uint8_t> read(unsigned offset) const noexcept
    {
        if(offset<0x48) {
            for(unsigned i=0;i<global.size();++i) if(offset==globalOffsets[i]) return global[i];
        } else if(offset<0x748) {
            const auto part=(offset-0x48)/0x70,field=(offset-0x48)%0x70;
            for(unsigned i=0;i<partOffsets.size();++i) if(field==partOffsets[i]) return parts[part][i];
        }
        return {};
    }
    bool write(unsigned offset,uint8_t value) noexcept
    {
        if(offset<0x48) {
            for(unsigned i=0;i<global.size();++i) if(offset==globalOffsets[i]) {global[i]=value;return true;}
        } else if(offset<0x748) {
            const auto part=(offset-0x48)/0x70,field=(offset-0x48)%0x70;
            for(unsigned i=0;i<partOffsets.size();++i) if(field==partOffsets[i]) {parts[part][i]=value;return true;}
        }
        return false;
    }
};

// GS bulk48 is a wire format, not native engine memory. Decode directly into
// the owning configuration fields; never retain a second emulated RAM image.
struct BulkSystemData
{
    template<class Store>
    static bool write(std::span<const uint8_t> payload,Store&& store) noexcept
    {
        if(payload.size()<3 || payload[0]!=0x48) return false;
        const unsigned offset=unsigned(payload[1])*64+(payload[2]>>1);
        if(offset>=0x748) return false;
        const auto bytes=payload.subspan(3);
        // EE9 yields FF on exhaustion. The loop writes once even if empty,
        // and clips at8748 rather than rejecting the already valid prefix.
        const auto count=std::min<std::size_t>(0x748-offset,std::max<std::size_t>(1,(bytes.size()+1)/2));
        for(std::size_t i=0;i<count;++i) {
            const auto high=2*i<bytes.size() ? bytes[2*i] : uint8_t(255);
            const auto low=2*i+1<bytes.size() ? bytes[2*i+1] : uint8_t(255);
            store(unsigned(offset+i),uint8_t((high<<4)+low));
        }
        return true;
    }
};

// Owned03:ca00..d147 data used by04:1e6a. This is configuration, not a
// serialized engine or a substitute for reset's voice/FX ordering.
struct SystemDefaults
{
    std::array<uint8_t,0x748> bytes{};
    std::array<uint8_t,32> identity{}; // Immutable firmware identification, not bulk settings.
    MasterControls master() const noexcept
    { return {uint16_t((bytes[0]<<8)|bytes[1]),bytes[2],bytes[5],bytes[6],bytes[3],bytes[4]}; }

    PartSettings parts(bool afterReset = false) const noexcept
    {
        PartSettings result;
        for (unsigned i = 0; i < 16; ++i)
        {
            const auto* p = bytes.data()+0x48+0x70*i;
            auto& part = result.parts[i];
            part.bank=part.bankSelect=p[0]; part.controls.program=p[1];
            result.routing[i] = {p[4],uint16_t((p[2]<<8)|p[3]|(afterReset ? 0x8000 : 0)),p[5]};
            part.keyShift=p[6]; part.fineTune=p[7]; part.controls.volume=p[8]; part.controls.pan=p[9];
            part.velocity={p[10],p[11]}; part.keyRange={p[12],p[13]};
            part.controls.chorus=p[14]; part.controls.reverb=p[15];
            std::copy_n(p+0x10,8,part.controls.tone.values.begin());
            std::copy_n(p+0x1a,12,part.scale.begin());
        }
        return result;
    }

    PartControllerState controllers() const noexcept
    {
        PartControllerState result;
        constexpr std::array<unsigned,11> columns{0,1,2,4,5,6,7,8,9,10,11};
        constexpr std::array<unsigned,5> sources{0x28,0x34,0x40,0x58,0x64};
        for (unsigned i = 0; i < 16; ++i)
        {
            const auto* p = bytes.data()+0x48+0x70*i;
            auto& part = result.parts[i];
            part.assignedControllers={p[0x26],p[0x27]};
            for (unsigned column=0;column<11;++column)
            {
                part.sensitivity[column]=p[0x4c+columns[column]];
                for (unsigned source=0;source<5;++source)
                    part.sourceSensitivity[source][column]=p[sources[source]+columns[column]];
            }
        }
        return result;
    }
};
}
