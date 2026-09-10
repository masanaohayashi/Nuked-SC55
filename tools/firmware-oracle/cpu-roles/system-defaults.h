#pragma once
#include "sc55_sound_data_import.h"

inline void VerifySystemDefaults(Emulator& emu)
{
    auto& cpu=emu.GetMCU();
    const std::vector<uint8_t> rom1(cpu.rom1,cpu.rom1+32768),rom2(cpu.rom2,cpu.rom2+262144);
    const auto defaults=sc55::ImportSystemDefaults(rom1,rom2);
    const auto run=[&](unsigned duration) { const auto end=cpu.cycles+duration; while(cpu.cycles<end) emu.Step(); };
    run(120000000);
    for (unsigned reset=0;reset<2;++reset)
    {
        if (reset) { const uint8_t gs[]{0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7};
            emu.PostMIDI(gs); run(10000000); }
        for (unsigned i=0;i<defaults.bytes.size();++i)
        {
            auto expected=defaults.bytes[i];
            if (reset && i>=0x48 && (i-0x48)%0x70==2) expected|=0x80;
            if (MCU_Read(cpu,0x8000+i)!=expected)
            {
                std::printf("Defaults mismatch reset=%u address=%04x ROM=%02x H8=%02x\n",
                    reset,0x8000+i,expected,MCU_Read(cpu,0x8000+i));
                throw std::runtime_error("System default image mismatch");
            }
        }
        const auto parts=defaults.parts(reset!=0); const auto master=defaults.master();
        if (master.volume!=MCU_Read(cpu,0x8002) || master.tune!=MCU_Read16(cpu,0x8000))
            throw std::runtime_error("Master default decode mismatch");
        const auto controls=defaults.controllers();
        constexpr std::array<unsigned,11> columns{0,1,2,4,5,6,7,8,9,10,11};
        constexpr std::array<unsigned,5> sources{0x28,0x34,0x40,0x58,0x64};
        for (unsigned part=0;part<16;++part)
        {
            const auto base=0x8048+part*0x70;
            if (parts.routing[part].flags!=MCU_Read16(cpu,base+2)
                || parts.routing[part].channel!=MCU_Read(cpu,base+4))
                throw std::runtime_error("Part default decode mismatch");
            for (unsigned column=0;column<11;++column)
            {
                if (controls.parts[part].sensitivity[column]!=MCU_Read(cpu,base+0x4c+columns[column]))
                    throw std::runtime_error("Poly pressure default mismatch");
                for (unsigned source=0;source<5;++source)
                    if (controls.parts[part].sourceSensitivity[source][column]!=MCU_Read(cpu,base+sources[source]+columns[column]))
                        throw std::runtime_error("Controller default mismatch");
            }
        }
    }
    std::puts("System defaults PASS: complete 1848-byte image at boot/GS reset and decoded controller rows");
}
