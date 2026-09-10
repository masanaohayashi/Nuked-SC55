#pragma once
#include "sc55_sound_data_import.h"

inline void VerifyEffectsSettings(Emulator& emu)
{
    auto& cpu=emu.GetMCU();
    const auto tables=sc55::ImportEffectsTables(
        std::vector<uint8_t>(cpu.rom1,cpu.rom1+32768),std::vector<uint8_t>(cpu.rom2,cpu.rom2+262144));
    const auto run=[&](unsigned cycles) { const auto end=cpu.cycles+cycles; while(cpu.cycles<end) emu.Step(); };
    run(120000000);
    sc55::EffectsSettings settings;
    for (unsigned i=0;i<6;++i) settings.reverb[i]=MCU_Read(cpu,0x802b+i);
    for (unsigned i=0;i<7;++i) settings.chorus[i]=MCU_Read(cpu,0x8033+i);
    settings.reverbMacro=MCU_Read(cpu,0x802a); settings.chorusMacro=MCU_Read(cpu,0x8032);
    unsigned checked=0;
    const auto check=[&](std::vector<uint8_t> payload) {
        settings.write(payload,tables);
        std::vector<uint8_t> packet{0xf0,0x41,0x10,0x42,0x12};
        unsigned sum=0;
        for (auto byte:payload) { packet.push_back(byte); sum+=byte; }
        packet.push_back(uint8_t((-sum)&127)); packet.push_back(0xf7);
        emu.PostMIDI(packet); run(2000000);
        bool match=settings.reverbMacro==MCU_Read(cpu,0x802a) && settings.chorusMacro==MCU_Read(cpu,0x8032);
        for (unsigned i=0;i<6;++i) match &= settings.reverb[i]==MCU_Read(cpu,0x802b+i);
        for (unsigned i=0;i<7;++i) match &= settings.chorus[i]==MCU_Read(cpu,0x8033+i);
        if (!match) {
            std::printf("Effects settings mismatch case=%u address=%02x\n",checked,payload[2]);
            std::printf("macros %u/%u %u/%u\n",settings.reverbMacro,MCU_Read(cpu,0x802a),settings.chorusMacro,MCU_Read(cpu,0x8032));
            for (unsigned i=0;i<6;++i) std::printf("R%u %u/%u\n",i,settings.reverb[i],MCU_Read(cpu,0x802b+i));
            for (unsigned i=0;i<7;++i) std::printf("C%u %u/%u\n",i,settings.chorus[i],MCU_Read(cpu,0x8033+i));
            throw std::runtime_error("Native GS effects settings differ from H8");
        }
        ++checked;
    };
    for (unsigned address=0x30;address<=0x3f;++address)
        if (address!=0x37) for (uint8_t value:{uint8_t(0),uint8_t(7),uint8_t(127)})
            check({0x40,1,uint8_t(address),value});
    for (uint8_t preset=0;preset<8;++preset) {
        check({0x40,1,0x30,preset}); check({0x40,1,0x38,preset});
        // Macro must restore a manually changed field even when unchanged.
        check({0x40,1,0x33,0}); check({0x40,1,0x30,preset});
    }
    check({0x40,1,0x31,3,2,17,29,51,63});
    check({0x40,1,0x39,2,17,51,27,80,9,127});
    check({0x40,1,0x30,1,2,3});
    check({0x40,1,0x36,51,12,7});
    check({0x40,1,0x37,12,7});
    check({0x40,1,0x33});
    std::printf("Native GS effects settings PASS: %u transactions, all macros and scalar fields\n",checked);
}
