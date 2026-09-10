#pragma once
#include <vector>

// Read-only observation of actual task8 group completion, including its
// non-returning envelope exits. State persists across MIDI capture windows.
struct ControlGroupProbe
{
    enum class Kind { begin, group, end, groupBegin, readback };
    struct Voice { uint16_t stage; uint8_t part,key,activity; };
    struct Event { unsigned frame; Kind kind; uint8_t ticks; uint32_t visited,updated; std::array<Voice,24> voices{}; uint8_t slot=255; };
    bool captureReadback=false;
    bool inPass=false, inGroup=false;
    uint32_t groupUpdated=0;
    void observe(mcu_t& cpu,uint64_t windowStart,std::vector<Event>& events)
    {
        if(cpu.cp!=0 || MCU_Read16(cpu,0xfdca)!=8) return;
        const auto append=[&](Kind kind,uint32_t visited=0) {
            events.push_back({unsigned((cpu.cycles-windowStart+624)/625),kind,
                uint8_t(MCU_Read16(cpu,0xac5a)),visited,kind==Kind::group ? groupUpdated : 0});
            if(kind==Kind::group || (captureReadback && (kind==Kind::groupBegin || kind==Kind::readback))) for(unsigned slot=0;slot<24;++slot) {
                const auto base=MCU_Read16(cpu,0x676a+slot*2);
                const auto group=MCU_Read(cpu,0xa330+slot);
                events.back().voices[slot]={MCU_Read16(cpu,base),MCU_Read(cpu,0xa318+slot),
                    group<24 ? MCU_Read(cpu,0xa2e8+group) : uint8_t(255),MCU_Read(cpu,0xac42+slot)};
            }
        };
        if(cpu.pc==0x5b0b && !inPass) {
            inPass=true; inGroup=false; append(Kind::begin);
        }
        if(!inPass) return;
        if((cpu.pc==0x5b73 || cpu.pc==0x5bb4) && !inGroup) {
            inGroup=true; groupUpdated=0; append(Kind::groupBegin);
        }
        if(inGroup && cpu.pc==0x318c) {
            const auto slot=MCU_Read16(cpu,uint16_t(cpu.r[0]-2));
            if(slot<24) groupUpdated|=1u<<slot;
        }
        if(inGroup && captureReadback && cpu.pc==0x3212) {
            const auto slot=MCU_Read16(cpu,uint16_t(cpu.r[0]-2));
            if(slot>=24) throw std::runtime_error("Invalid control readback owner");
            append(Kind::readback);events.back().slot=uint8_t(slot);
        }
        if(cpu.pc==0x5b5c && inGroup) {
            uint32_t visited=0;
            for(unsigned slot=0;slot<24;++slot) {
                const auto base=MCU_Read16(cpu,0x676a+slot*2);
                if(MCU_Read(cpu,uint16_t(base-26))) visited|=1u<<slot;
            }
            append(Kind::group,visited); inGroup=false;
        }
        if(cpu.pc==0x5b70) { append(Kind::end); inPass=false; inGroup=false; }
    }
};
