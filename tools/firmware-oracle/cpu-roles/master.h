#pragma once
#include "sc55_sysex.h"

inline void VerifyNativeMaster(Emulator& emu)
{
    auto& cpu = emu.GetMCU();
    auto run = [&](unsigned cycles) { const auto end=cpu.cycles+cycles; while(cpu.cycles<end) emu.Step(); };
    run(120000000);
    sc55::MasterControls native;
    sc55::SysExReceiver receiver;
    sc55::MidiDecoder decoder;
    unsigned checked = 0;
    for (const auto& payload : {
            std::vector<uint8_t>{0x40,0,4,37}, {0x40,0,6,0}, {0x40,0,6,127},
            {0x40,0,5,0}, {0x40,0,5,127}, {0x40,0,0x7e,85}, {0x40,0,6,23,84}, {0x40,0,0,0,4,0,0},
            {0x40,0,0,0,0,0,0}, {0x40,0,0,15,15,15,15},
            {0x40,0,0,0x71,0x22,0x33,0x44}, {0x40,0,0,0,4,0,0},
            {0x40,0,4,37,64,0}, {0x40,0,0,0,5,0,0,100,64,64}}) {
        std::vector<uint8_t> message{0xf0,0x41,0x10,0x42,0x12};
        unsigned sum = 0;
        for (auto byte : payload) { message.push_back(byte); sum += byte; }
        message.push_back(uint8_t((128-(sum&127))&127)); message.push_back(0xf7);
        bool applied = false;
        decoder.push(message,[&](auto event) {
            const auto packet = receiver.receive(event);
            if (packet.status == sc55::SysExReceiver::Status::roland)
                applied = native.write(packet.payload) == sc55::MasterControls::WriteResult::applied;
        });
        emu.PostMIDI(message); run(1000000);
        const auto address = payload[2];
        const unsigned expected = address == 0 ? MCU_Read16(cpu,0x8000) : MCU_Read(cpu,address==0x7e ? 0x8003 : 0x8000+address-(address==4?2:0));
        const unsigned actual = address == 0 ? native.tune : address == 4 ? native.volume : address == 5 ? native.keyShift : address==0x7e ? native.portamentoController : native.pan;
        const bool shouldApply = address != 0 || payload.size() == 7;
        if (applied != shouldApply || actual != expected || native.tune != MCU_Read16(cpu,0x8000)
            || native.volume != MCU_Read(cpu,0x8002) || native.keyShift != MCU_Read(cpu,0x8005)
            || native.pan != MCU_Read(cpu,0x8006) || native.portamentoController!=MCU_Read(cpu,0x8003)) {
            std::printf("MASTER mismatch address=%u native=%u H8=%u\n",address,actual,expected);
            std::printf("case=%u tune=%u/%u volume=%u/%u shift=%u/%u pan=%u/%u\n",checked,
                native.tune,MCU_Read16(cpu,0x8000),native.volume,MCU_Read(cpu,0x8002),
                native.keyShift,MCU_Read(cpu,0x8005),native.pan,MCU_Read(cpu,0x8006));
            throw std::runtime_error("Native master differs from firmware");
        }
        ++checked;
    }
    std::printf("Native master PASS: %u real SysEx writes match H8 (including clamping and non-nibble data)\n",checked);
}
