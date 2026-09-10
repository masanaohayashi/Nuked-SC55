#pragma once
#include "pcm.h"

// Firmware-only observation: no synthetic IRQ, register writes, or native path.
inline void TraceEffects(Emulator& emu)
{
    auto& cpu = emu.GetMCU();
    auto word = [&](unsigned address) { return MCU_Read16(cpu,address); };
    auto run = [&](unsigned duration,const char* label) {
        const auto start = cpu.cycles, end = start + duration;
        unsigned previousReverb = 0xffff, previousChorus = 0xffff;
        std::map<unsigned,unsigned> states;
        while (cpu.cycles < end) {
            if (cpu.cp == 0 && (cpu.pc == 0x604a || cpu.pc == 0x6412)) {
                const bool reverb = cpu.pc == 0x604a;
                const auto state = word(reverb ? 0xcb6c : 0xcb6e);
                auto& previous = reverb ? previousReverb : previousChorus;
                ++states[(reverb ? 0 : 0x100) + state];
                if (previous != state)
                    std::printf("FX %s %s cycles=%llu state=%u\n",label,
                        reverb ? "reverb" : "chorus",
                        (unsigned long long)(cpu.cycles-start),state);
                previous = state;
            }
            emu.Step();
        }
        for (auto [state,count] : states)
            std::printf("FX_COUNT %s %s state=%u calls=%u\n",label,
                state < 0x100 ? "reverb" : "chorus",state & 255,count);
        std::printf("FX_END %s reverb=%u chorus=%u\n",label,word(0xcb6c),word(0xcb6e));
        if (word(0xcb6c) != 0 || word(0xcb6e) != 0)
            throw std::runtime_error("effects transition did not settle");
    };
    run(120000000,"boot");
    for (auto item : {std::array<unsigned,2>{0x33,17}, {0x31,6}, {0x3a,17}, {0x3c,27}}) {
        const uint8_t message[]{0xf0,0x41,0x10,0x42,0x12,0x40,1,
            uint8_t(item[0]),uint8_t(item[1]),uint8_t((128-((0x41+item[0]+item[1])&127))&127),0xf7};
        char label[32]; std::snprintf(label,sizeof(label),"set-4001%02x",item[0]);
        const auto before = emu.GetMCU().pcm->ram2;
        // ram2 is a C array: copy before advancing the emulator.
        std::array<std::array<uint16_t,16>,4> saved{};
        for (unsigned bank=28;bank<32;++bank)
            for (unsigned i=0;i<16;++i) saved[bank-28][i]=before[bank][i];
        emu.PostMIDI(message); run(20000000,label);
        for (unsigned bank=28;bank<32;++bank)
            for (unsigned i=0;i<16;++i)
                if (saved[bank-28][i] != cpu.pcm->ram2[bank][i])
                    std::printf("FX_RAM %s bank=%u index=%u old=%04x new=%04x\n",
                        label,bank,i,saved[bank-28][i],cpu.pcm->ram2[bank][i]);
    }
}
