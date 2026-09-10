#pragma once
#include "sc55_part_settings.h"
#include "sc55_note_start.h"

inline void VerifyNativeParts(Emulator& emu)
{
    auto& cpu = emu.GetMCU();
    auto run = [&](unsigned cycles) { const auto end=cpu.cycles+cycles; while(cpu.cycles<end) emu.Step(); };
    run(120000000);
    sc55::PartSettings native;
    sc55::PartControllerState controllers;
    unsigned checks = 0;
    auto compare = [&](unsigned part) {
        const unsigned base = 0x8048+0x70*part;
        auto& p = native.parts[part];
        const auto check = [&](unsigned value,unsigned address,bool word = false) {
            const unsigned expected = word ? MCU_Read16(cpu,base+address) : MCU_Read(cpu,base+address);
            if (value != expected) {
                std::printf("PART mismatch part=%u offset=%02x native=%u H8=%u\n",part,address,value,expected);
                throw std::runtime_error("Native part settings differ from firmware");
            }
        };
        check(native.routing[part].channel,4); check(native.routing[part].flags,2,true);
        check(p.controls.volume,8); check(p.controls.pan,9); check(p.controls.chorus,14); check(p.controls.reverb,15);
        check(p.keyShift,6); check(p.fineTune,7); check(p.velocity.depth,10); check(p.velocity.offset,11);
        check(p.keyRange.low,12); check(p.keyRange.high,13);
        for (unsigned i=0;i<12;++i) check(p.scale[i],0x1a+i);
        for(unsigned i=0;i<8;++i) check(p.controls.tone.values[i],0x10+i);
        ++checks;
    };
    for (unsigned part=0;part<16;++part) compare(part);
    auto send = [&](const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> message{0xf0,0x41,0x10,0x42,0x12};
        unsigned sum=0;
        for (auto byte:payload) {message.push_back(byte);sum+=byte;}
        message.push_back(uint8_t((128-(sum&127))&127)); message.push_back(0xf7);
        // This probe compares configuration only; engine side effects are
        // checked separately by native-player and the live all-notes probe.
        if (native.write(payload,[](unsigned) { return true; },
            [](unsigned,uint8_t,uint8_t) { return false; },
            &controllers.parts[payload[1]&15].assignedControllers) != sc55::PartSettings::WriteResult::applied)
            throw std::runtime_error("Native part rejected test input");
        emu.PostMIDI(message); run(1000000);
        compare(payload[1]&15);
    };
    for (uint8_t address=3;address<=0x12;++address) {
        send({0x40,0x11,address,0}); send({0x40,0x11,address,127});
    }
    for (uint8_t part : {uint8_t(0),uint8_t(1),uint8_t(9),uint8_t(15)}) {
        send({0x40,uint8_t(0x10|part),0x16,127});
        send({0x40,uint8_t(0x10|part),0x17,0,0});
        send({0x40,uint8_t(0x10|part),0x17,15,15});
        send({0x40,uint8_t(0x10|part),0x17,0x71,0x22});
        send({0x40,uint8_t(0x10|part),0x17,8,0});
        send({0x40,uint8_t(0x10|part),0x19,23,34,45,0,72,48});
        send({0x40,uint8_t(0x10|part),0x21,25,26});
        send({0x40,uint8_t(0x10|part),0x1f,19,74,25,26});
        if(controllers.parts[part].assignedControllers[0]!=MCU_Read(cpu,0x8048+0x70*part+0x26)
            || controllers.parts[part].assignedControllers[1]!=MCU_Read(cpu,0x8048+0x70*part+0x27))
            throw std::runtime_error("Assigned controllers differ from ROM");
        send({0x40,uint8_t(0x10|part),0x30,0,127,127,0,127,0,127,0});
        send({0x40,uint8_t(0x10|part),0x40,65,66,67,68,69,70,71,72,73,74,75,76});
        send({0x40,uint8_t(0x10|part),2,127});
        send({0x40,uint8_t(0x10|part),2,2});
    }
    for(unsigned group=0;group<6;++group) {
        std::vector<uint8_t> payload{0x40,0x21,uint8_t(group*16)};
        for(unsigned i=0;i<11;++i) payload.push_back(uint8_t(i==0 ? (group&1 ? 127 : 0) : 13*i%128));
        if(controllers.writeSettings(payload)!=sc55::PartControllerState::WriteResult::applied)
            throw std::runtime_error("Controller configuration rejected");
        std::vector<uint8_t> message{0xf0,0x41,0x10,0x42,0x12}; unsigned sum=0;
        for(auto byte:payload) {message.push_back(byte); sum+=byte;}
        message.push_back(uint8_t((128-(sum&127))&127)); message.push_back(0xf7);
        emu.PostMIDI(message); run(1000000);
        const auto& row=group==3 ? controllers.parts[1].sensitivity
            : controllers.parts[1].sourceSensitivity[group<3 ? group : group-1];
        for(unsigned i=0;i<11;++i)
            if(row[i]!=MCU_Read(cpu,0x80b8+0x28+12*group+i+(i>=3 ? 1 : 0)))
                throw std::runtime_error("Controller sensitivity differs from ROM");
        ++checks;
    }
    std::printf("Native parts PASS: %u full supported-field comparisons with H8\n",checks);
}
