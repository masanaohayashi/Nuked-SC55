#pragma once
#include "sc55_part_settings.h"
inline void VerifyRpn(Emulator& emu)
{
    auto& cpu=emu.GetMCU(); sc55::PartSettings native;
    const auto run=[&](unsigned cycles) { auto end=cpu.cycles+cycles; while(cpu.cycles<end) emu.Step(); };
    run(120000000);
    unsigned count=0;
    const auto send=[&](uint8_t cc,uint8_t value) {
        const uint8_t message[]{0xb0,cc,value};
        sc55::MidiDecoder decoder;
        decoder.push(message,[&](auto event) { native.applyScalar(event); });
        emu.PostMIDI(message); run(200000);
        const auto& c=native.parts[1].controls;
        if (uint16_t(c.finePitch())!=MCU_Read16(cpu,0xab78)
            || unsigned(c.bendRange)+64!=MCU_Read(cpu,0x80ec)
            || uint8_t(c.coarseTuning)!=MCU_Read(cpu,0xab47))
            throw std::runtime_error("Native RPN tuning differs from H8");
        ++count;
    };
    send(101,0);
    send(100,1); send(38,127); // LSB first after boot's zero entry latch.
    for (uint8_t rpn=0;rpn<3;++rpn) {
        send(100,rpn);
        for (uint8_t value:{uint8_t(0),uint8_t(1),uint8_t(24),uint8_t(64),uint8_t(88),uint8_t(127)}) {
            send(6,value); send(38,127); send(38,0); send(38,63);
        }
    }
    std::printf("Native RPN PASS: %u messages, bend range/fine/coarse match H8\n",count);
}
