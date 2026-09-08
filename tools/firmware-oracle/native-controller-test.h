#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace
{
inline void verifyNativeControllers(mcu_t& cpu)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    for (unsigned test = 0; test < 1024; ++test)
    {
        // Only ordinary SRAM is randomized; no device state or ROM is changed.
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.sr = 0x070f;
        cpu.pc = 0x5c20;
        cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        cpu.r[1] = uint16_t(test%24);
        cpu.r[0] = mcu_native::ReadWord(cpu,0x676a+2*cpu.r[1]);
        const unsigned part = test%16;
        MCU_Write(cpu,0xc8e4+cpu.r[1],uint8_t(part));
        MCU_Write(cpu,0xc8fc+cpu.r[1],60);
        const auto partBase = mcu_native::ReadWord(cpu,0x74a4+part*2);
        // Alternate full wrapping-word inputs with normal MIDI-range/zero inputs.
        if (test < 512) {
            for (unsigned i = 0; i < 11; ++i) {
                MCU_Write(cpu,partBase+0x4c+i+(i>=3),uint8_t(test%128));
                for (unsigned source = 0; source < 5; ++source)
                    MCU_Write16(cpu,0x9060+source*0x160+i*0x20+part*2,0);
            }
            MCU_Write(cpu,0x9740+128*part+60,uint8_t(test/4));
        }
        const auto originalSR = cpu.sr;
        std::array<uint16_t,8> originalRegisters;
        std::copy(std::begin(cpu.r),std::end(cpu.r),originalRegisters.begin());
        const std::vector<uint8_t> originalMemory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryPrepareControllers(cpu))
            throw std::runtime_error("Controller native gate rejected fixture");
        std::array<uint16_t,8> nativeRegisters;
        std::copy(std::begin(cpu.r),std::end(cpu.r),nativeRegisters.begin());
        const auto nativeSR = cpu.sr;
        const auto count = cpu.native_debt+1;
        const std::vector<uint8_t> nativeMemory(std::begin(cpu.sram),std::end(cpu.sram));
        std::copy(originalMemory.begin(),originalMemory.end(),std::begin(cpu.sram));
        std::copy(originalRegisters.begin(),originalRegisters.end(),std::begin(cpu.r));
        cpu.sr = originalSR;
        cpu.pc = 0x5c20;
        cpu.native_debt = 0;
        unsigned steps = 0;
        while (cpu.pc != 0x5ff4 && ++steps < 1000) {
            auto opcode = MCU_ReadCodeAdvance(cpu);
            MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.sr != nativeSR
            || !std::equal(nativeRegisters.begin(),nativeRegisters.end(),std::begin(cpu.r))
            || !std::equal(nativeMemory.begin(),nativeMemory.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Controllers case %u steps %u/%u SR %04x/%04x\n",test,steps,count,cpu.sr,nativeSR);
            for (unsigned i = 0; i < 8; ++i)
                std::fprintf(stderr,"r%u=%04x/%04x ",i,cpu.r[i],nativeRegisters[i]);
            throw std::runtime_error("Controller conversion differs at 5ff4");
        }
    }
    for (unsigned guard = 0; guard < 9; ++guard) {
        cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.sr = 0x0700;
        cpu.pc = 0x5c20;
        cpu.native_v121_enabled = true;
        cpu.r[1] = 0;
        cpu.r[0] = mcu_native::ReadWord(cpu,0x676a);
        MCU_Write(cpu,0xc8e4,0);
        MCU_Write(cpu,0xc8fc,60);
        switch (guard) {
            case 0: cpu.sr = 0; break;
            case 1: cpu.sr |= STATUS_T; break;
            case 2: cpu.cp = 1; break;
            case 3: cpu.dp = 1; break;
            case 4: cpu.ep = 1; break;
            case 5: cpu.r[0] += 2; break;
            case 6: cpu.r[1] = 24; break;
            case 7: MCU_Write(cpu,0xc8e4,16); break;
            case 8: cpu.native_v121_enabled = false; break;
        }
        const std::vector<uint8_t> before(std::begin(cpu.sram),std::end(cpu.sram));
        if (mcu_native::TryPrepareControllers(cpu) || cpu.pc != 0x5c20 || cpu.native_debt != 0
            || !std::equal(before.begin(),before.end(),std::begin(cpu.sram)))
            throw std::runtime_error("Native controller guard mutated state");
    }
    std::printf("Controllers: 1024 register/SR/SRAM/instruction-count cases and fallback guards matched\n");
}
}
