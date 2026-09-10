#pragma once
#include "sc55_part_settings.h"
#include "sc55_note_start.h"
#include "sc55_sysex.h"
#include "sc55_effects_control.h"
#include "sc55_rhythm_presets.h"
#include <algorithm>

namespace sc55
{
// One normal GS parameter response. Owned by the audio thread, with no RAM
// mirror or H8 execution. Bulk transfers use a separate paced transaction.
struct ParameterReply
{
    std::array<uint8_t,42> bytes{};
    unsigned size=0;
    enum class Result { ready, unsupported, invalidLength };

    Result prepare(std::span<const uint8_t> request,const MasterControls& master,
                   const PartSettings& parts,const PartControllerState& controllers) noexcept
    {
        size=0;
        if(request.size()!=6) return Result::invalidLength;
        if(request[0]!=0x40) return Result::unsupported;
        std::array<uint8_t,16> data{};
        unsigned count=1;
        const unsigned page=request[1],key=request[2],index=page&15;
        if(page==0) {
            switch(key) {
            case 0: count=4; for(unsigned i=0;i<4;++i) data[i]=uint8_t((master.tune>>(12-4*i))&15); break;
            case 4: data[0]=master.volume; break;
            case 5: data[0]=master.keyShift; break;
            case 6: data[0]=master.pan; break;
            case 0x7e: data[0]=master.portamentoController; break;
            default: return Result::unsupported;
            }
        } else if((page&0xf0)==0x10) {
            const auto& p=parts.parts[index]; const auto& r=parts.routing[index];
            const auto& c=controllers.parts[index];
            if(key>=3 && key<=0x12) {
                constexpr std::array<uint16_t,16> masks{0x4000,0x2000,0x1000,0x0800,
                    0x0400,0x0200,0x0001,0x8000,0x0002,0x0004,0x0008,0x0010,
                    0x0020,0x0040,0x0080,0x0100};
                data[0]=(r.flags&masks[key-3])!=0;
            } else if(key>=0x30 && key<=0x37) data[0]=p.controls.tone.values[key-0x30];
            else switch(key) {
            case 0: count=2; data[0]=p.bank; data[1]=p.controls.program; break;
            case 2: data[0]=r.channel; break;
            case 0x13: data[0]=r.noteFlags>>7; break;
            case 0x14: data[0]=r.noteFlags&3; break;
            // 04:1ac6 tests rhythm-enable bit4, then map-select bit5.
            case 0x15: data[0]=!(r.noteFlags&0x10) ? 0 : (r.noteFlags&0x20) ? 1 : 2; break;
            case 0x16: data[0]=p.keyShift; break;
            case 0x17: count=2; data[0]=p.fineTune>>4; data[1]=p.fineTune&15; break;
            case 0x19: data[0]=p.controls.volume; break;
            case 0x1a: data[0]=p.velocity.depth; break;
            case 0x1b: data[0]=p.velocity.offset; break;
            case 0x1c: data[0]=p.controls.pan; break;
            case 0x1d: data[0]=p.keyRange.low; break;
            case 0x1e: data[0]=p.keyRange.high; break;
            case 0x1f: case 0x20: data[0]=c.assignedControllers[key-0x1f]; break;
            case 0x21: data[0]=p.controls.chorus; break;
            case 0x22: data[0]=p.controls.reverb; break;
            case 0x40: count=12; std::copy(p.scale.begin(),p.scale.end(),data.begin()); break;
            default: return Result::unsupported;
            }
        } else if((page&0xf0)==0x20) {
            const unsigned source=key>>4,column=key&15;
            if(source>=6 || column>=11) return Result::unsupported;
            const auto& c=controllers.parts[index];
            data[0]=source==3 ? c.sensitivity[column] : c.sourceSensitivity[source>3 ? source-1 : source][column];
        } else return Result::unsupported;
        return finish(request,std::span(data).first(count));
    }

    Result prepareSystem(std::span<const uint8_t> request,
        std::span<const uint8_t,16> name,const VoiceCapacityPolicy& capacity,
        const EffectsSettings& effects,std::span<const RhythmPresetTable::Record> drums,
        uint8_t resetRegister) noexcept
    {
        size=0;
        if(request.size()!=6) return Result::invalidLength;
        const unsigned page=request[1],key=request[2];
        uint8_t value=0;
        if(request[0]==0x40 && page==0 && key==0x7f) {
            // d6e8 record: scalar at 8004. Reading does not request a reset.
            value=resetRegister;
        } else if(request[0]==0x40 && page==1) {
            if(key==0) return finish(request,name);
            if(key==0x10) return finish(request,capacity.reserves);
            if(key==0x20) value=capacity.startPartControl;
            else if(key==0x30) value=effects.reverbMacro;
            else if(key>=0x31 && key<=0x36) value=effects.reverb[key-0x31];
            else if(key==0x38) value=effects.chorusMacro;
            else if(key>=0x39 && key<=0x3f) value=effects.chorus[key-0x39];
            else return Result::unsupported;
        } else if(request[0]==0x41) {
            const unsigned map=page>>4,field=page&15;
            if(map>=drums.size() || field>8 || key>=128 || (field==0 && key!=0))
                return Result::unsupported;
            const auto& drum=drums[map];
            if(field==0) return finish(request,std::span(drum).subspan(0x480,12));
            constexpr std::array<unsigned,7> offsets{0x480,0x180,0x100,0x200,0x280,0x300,0x380};
            if(field<7) value=drum[offsets[field]+key];
            else value=(drum[0x400+key]&(field==7 ? 1 : 16))!=0;
        } else return Result::unsupported;
        return finish(request,std::span(&value,1));
    }

    template<class ReadWave>
    Result prepareInformation(std::span<const uint8_t> request,
        std::span<const uint8_t,32> identity,ReadWave&& readWave) noexcept
    {
        size=0;
        if(request.size()<3) return Result::invalidLength;
        if(request[0]!=0x40 || request[1]<0x30 || request[1]>=0x40)
            return Result::unsupported;
        std::array<uint8_t,32> data;
        data.fill(' ');
        if(request[2]==0) std::copy(identity.begin(),identity.end(),data.begin());
        else if(request[2]==0x20) {
            const unsigned base=unsigned(request[1]&15)<<20;
            for(unsigned i=0;i<8;++i) data[i]=readWave(base+0x20+i);
            for(unsigned i=0;i<10;++i) data[10+i]=readWave(base+0x30+i);
            data[22]=readWave(base+0x44)==4 ? '4' : '8';
        } else return Result::unsupported;
        // 04:1891..1929 does not consume/validate the requested size.
        encode(request,data);
        return Result::ready;
    }

private:
    Result finish(std::span<const uint8_t> request,std::span<const uint8_t> data) noexcept
    {
        const auto count=unsigned(data.size());
        // 04:17b6 sums all three request bytes; the read handlers require
        // exactly one complete record, not a range of consecutive records.
        if(unsigned(request[3])+request[4]+request[5]!=count) return Result::invalidLength;
        encode(request,data);
        return Result::ready;
    }
    void encode(std::span<const uint8_t> request,std::span<const uint8_t> data) noexcept
    {
        const auto count=unsigned(data.size());
        bytes[0]=0xf0; bytes[1]=0x41; bytes[2]=0x10; bytes[3]=0x42; bytes[4]=0x12;
        unsigned sum=0;
        for(unsigned i=0;i<3;++i) { bytes[5+i]=request[i]; sum+=request[i]; }
        for(unsigned i=0;i<count;++i) { bytes[8+i]=data[i]; sum+=data[i]; }
        bytes[8+count]=uint8_t(-sum)&127; bytes[9+count]=0xf7; size=count+10;
    }
};
}
