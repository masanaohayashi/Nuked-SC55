#pragma once
#include "sc55_voice_allocator.h"

// Live command-boundary comparison, not manufactured note allocation state.
inline void VerifyAllNotesOff(Emulator& emu)
{
    auto& cpu = emu.GetMCU();
    unsigned checks = 0;
    std::optional<sc55::VoiceAllocator> expected;
    auto visit = [&](auto& state,auto&& field) {
        auto row = [&](unsigned base,auto& values) {
            for (unsigned i=0;i<values.size();++i) field(base+i,values[i]);
        };
        row(0xa210,state.partHead);
        for(unsigned i=0;i<state.noteGroups.size();++i) {
            auto& group=state.noteGroups[i];
            field(0xa240+i,group.next); field(0xa270+i,group.status);
            field(0xa288+i,group.retirementFlags); field(0xa2d0+i,group.noteClass);
            field(0xa300+i,group.releaseFlags); field(0xa2e8+i,group.key);
        }
        row(0xa2b8,state.groups.tail);
        row(0xa3a8,state.groups.previous); row(0xa230,state.partFlags);
        for(unsigned i=0;i<state.allocations.size();++i) {
            field(0xa360+i,state.allocations[i].releaseRequested);
            field(0xa3e0+i,state.allocations[i].releaseCommand);
        }
    };
    auto run = [&](unsigned duration) {
        const auto end = cpu.cycles+duration;
        while (cpu.cycles<end) {
            if (cpu.cp == 0 && cpu.pc == 0x08a1) {
                expected.emplace();
                visit(*expected,[&](unsigned address,uint8_t& value) { value=MCU_Read(cpu,address); });
                const unsigned part=cpu.r[3]&255;
                std::array<uint8_t,16> retained{};
                for (unsigned i=0;i<16;++i) retained[i]=MCU_Read(cpu,0xa090+part*16+i);
                if (!expected->requestGroupReleases(part,(MCU_Read(cpu,0x804d+part*0x70)&16)!=0,
                    0x80,0,retained)) throw std::runtime_error("Native all-notes rejected live state");
            }
            if (expected && cpu.cp == 0 && cpu.pc == 0x08ab) {
                visit(*expected,[&](unsigned address,uint8_t& value) {
                    if (value != MCU_Read(cpu,address)) {
                        std::printf("ALL_NOTES mismatch %04x native=%u H8=%u\n",address,value,MCU_Read(cpu,address));
                        throw std::runtime_error("Native all-notes differs from H8");
                    }
                });
                ++checks; expected.reset();
            }
            emu.Step();
        }
    };
    auto send = [&](std::initializer_list<uint8_t> bytes) { emu.PostMIDI(std::span(bytes.begin(),bytes.size())); run(2000000); };
    run(120000000);
    send({0xc0,48});
    for (unsigned mode=0;mode<3;++mode) {
        send({0x90,60,100}); send({0x90,60,90}); send({0x90,64,80});
        if (mode==1) send({0xb0,64,127});
        if (mode==2) send({0xb0,66,127});
        send({0xb0,123,0}); send({0xb0,123,0});
        send({0xb0,64,0}); send({0xb0,66,0});
        run(40000000);
    }
    send({0x99,38,100}); send({0xb9,123,0});
    if (checks != 7 || expected) throw std::runtime_error("Missing live all-notes comparisons");
    std::printf("Native all-notes PASS: %u live commands, duplicate keys, hold, sostenuto, rhythm\n",checks);
}
