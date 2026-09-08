#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace
{
inline void verifyNativeLevel(mcu_t& cpu)
{
    cpu.native_v121_enabled = true;
    cpu.dev_register[DEV_RAMCR] |= 0x80;
    for (unsigned address = 0x8000; address < 0xe000; ++address) {
        MCU_Write(cpu,address,uint8_t(address));
        if (MCU_Read_Slow(cpu,address) != uint8_t(address))
            throw std::runtime_error("MK1 fast SRAM write differs");
        MCU_Write_Slow(cpu,address,uint8_t(address>>8));
        if (MCU_Read(cpu,address) != uint8_t(address>>8))
            throw std::runtime_error("MK1 fast SRAM read differs");
    }
    for (unsigned address = 0xfb80; address < 0xff80; ++address) {
        MCU_Write(cpu,address,uint8_t(address));
        if (MCU_Read_Slow(cpu,address) != uint8_t(address))
            throw std::runtime_error("MK1 fast internal RAM write differs");
    }
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    constexpr unsigned voice = 0xb000;
    for (unsigned test = 0; test < 4096; ++test) {
        cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.sr = 0x070f;
        cpu.pc = 0x309b;
        cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = voice; cpu.r[1] = 0; cpu.r[7] = (test & 1) ? 0xff00 : 0xd800;
        MCU_Write(cpu,0xc8e4,0);
        MCU_Write(cpu,0xab36,test == 0 ? 0 : uint8_t(next()));
        MCU_Write(cpu,0x8002,uint8_t(next()));
        MCU_Write16(cpu,voice+0x2e,0x8048);
        MCU_Write(cpu,0x8050,uint8_t(next()));
        MCU_Write16(cpu,voice+0x30,(test & 2) ? 0x9000 : 0);
        MCU_Write(cpu,0x9100,uint8_t(next()));
        for (int offset : {0x8a,-122,0x8e,-96,-88,0x96,-62})
            MCU_Write16(cpu,unsigned(int(voice)+offset),test < 256 ? 0 : next());
        if (test >= 256 && test < 599) {
            constexpr uint16_t edges[]{0,1,0x7f00,0x7fff,0x8000,0x8001,0xffff};
            const unsigned index = test-256;
            MCU_Write(cpu,0xab36,100); MCU_Write(cpu,0x8002,127); MCU_Write(cpu,0x8050,127);
            MCU_Write16(cpu,voice+0x30,0); MCU_Write16(cpu,voice+0x8a,0);
            for (int offset : {-122,-88}) MCU_Write16(cpu,unsigned(int(voice)+offset),edges[index%7]);
            for (int offset : {0x8e,0x96}) MCU_Write16(cpu,voice+offset,edges[(index/7)%7]);
            for (int offset : {-96,-62}) MCU_Write16(cpu,unsigned(int(voice)+offset),edges[index/49]);
        }
        const auto sr = cpu.sr;
        std::array<uint16_t,8> registers;
        std::copy(std::begin(cpu.r),std::end(cpu.r),registers.begin());
        const std::vector<uint8_t> sram(std::begin(cpu.sram),std::end(cpu.sram));
        const std::vector<uint8_t> ram(std::begin(cpu.ram),std::end(cpu.ram));
        if (!mcu_native::TryComposeLevelV121(cpu)) throw std::runtime_error("Native level rejected fixture");
        const auto nativeSR = cpu.sr, nativePC = cpu.pc;
        const auto count = cpu.native_debt+1;
        std::array<uint16_t,8> nativeRegisters;
        std::copy(std::begin(cpu.r),std::end(cpu.r),nativeRegisters.begin());
        const std::vector<uint8_t> nativeSram(std::begin(cpu.sram),std::end(cpu.sram));
        const std::vector<uint8_t> nativeRam(std::begin(cpu.ram),std::end(cpu.ram));
        std::copy(registers.begin(),registers.end(),std::begin(cpu.r));
        std::copy(sram.begin(),sram.end(),std::begin(cpu.sram));
        std::copy(ram.begin(),ram.end(),std::begin(cpu.ram));
        cpu.sr = sr; cpu.pc = 0x309b; cpu.native_debt = 0;
        unsigned steps = 0;
        while (cpu.pc != 0x30ea && cpu.pc != 0x3126 && cpu.pc != 0x312a && ++steps < 1000) {
            auto opcode = MCU_ReadCodeAdvance(cpu);
            MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.sr != nativeSR || cpu.pc != nativePC
            || !std::equal(nativeRegisters.begin(),nativeRegisters.end(),std::begin(cpu.r))
            || !std::equal(nativeSram.begin(),nativeSram.end(),std::begin(cpu.sram))
            || !std::equal(nativeRam.begin(),nativeRam.end(),std::begin(cpu.ram))) {
            std::fprintf(stderr,"Level case %u steps %u/%u SR %04x/%04x PC %04x/%04x\n",
                         test,steps,count,cpu.sr,nativeSR,cpu.pc,nativePC);
            for (unsigned i = 0; i < 8; ++i)
                std::fprintf(stderr,"r%u=%04x/%04x ",i,cpu.r[i],nativeRegisters[i]);
            throw std::runtime_error("Native level differs at RTS");
        }
    }
    for (unsigned guard = 0; guard < 12; ++guard) {
        cpu.cp = cpu.dp = cpu.ep = 0; cpu.sr = 0x0700; cpu.pc = 0x309b;
        cpu.native_v121_enabled = true;
        cpu.r[0] = voice; cpu.r[1] = 0; cpu.r[7] = 0xff00;
        cpu.dev_register[DEV_RAMCR] |= 0x80;
        MCU_Write(cpu,0xc8e4,0);
        MCU_Write16(cpu,voice+0x2e,0x8048);
        MCU_Write16(cpu,voice+0x30,0);
        switch (guard) {
            case 0: cpu.sr = 0; break;
            case 1: cpu.sr |= STATUS_T; break;
            case 2: cpu.dp = 1; break;
            case 3: cpu.cp = 1; break;
            case 4: cpu.ep = 1; break;
            case 5: cpu.r[0] = 0xd800; break;
            case 6: cpu.r[7] = 0xff01; break;
            case 7: cpu.dev_register[DEV_RAMCR] &= 0x7f; break;
            case 8: cpu.r[1] = 24; break;
            case 9: MCU_Write16(cpu,voice+0x2e,0xe000); break;
            case 10: MCU_Write16(cpu,voice+0x30,0xdf00); break;
            case 11: cpu.native_v121_enabled = false; break;
        }
        const std::vector<uint8_t> sram(std::begin(cpu.sram),std::end(cpu.sram));
        const std::vector<uint8_t> ram(std::begin(cpu.ram),std::end(cpu.ram));
        if (mcu_native::TryComposeLevelV121(cpu) || cpu.pc != 0x309b || cpu.native_debt != 0
            || !std::equal(sram.begin(),sram.end(),std::begin(cpu.sram))
            || !std::equal(ram.begin(),ram.end(),std::begin(cpu.ram)))
            throw std::runtime_error("Native level fallback mutated memory");
    }
    std::printf("Level: 4096 register/SR/RAM/SRAM/instruction-count cases and fallback guards matched; MK1 RAM fast path matched\n");
}
}
