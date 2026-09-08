#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace
{
inline void verifyNativeCutoff(mcu_t& cpu)
{
    cpu.native_v121_enabled = true;
    constexpr unsigned voice = 0x9400;
    unsigned cases = 0;
    auto check = [&](uint16_t control, uint16_t previous, uint8_t flag, uint16_t selector) {
        cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.sr = 0x070f;
        cpu.pc = 0x473c;
        cpu.native_debt = 0;
        for (unsigned i = 0; i < 8; ++i) cpu.r[i] = uint16_t(0x1111*i);
        cpu.r[0] = voice;
        MCU_Write16(cpu,voice+0x22,control);
        MCU_Write16(cpu,voice+0x24,previous);
        MCU_Write16(cpu,voice+0x26,0x1234);
        MCU_Write(cpu,voice+0x68,flag);
        MCU_Write16(cpu,voice-0x30,selector);
        const auto originalSR = cpu.sr;
        std::array<uint16_t,8> originalRegisters;
        std::copy(std::begin(cpu.r),std::end(cpu.r),originalRegisters.begin());
        const std::vector<uint8_t> originalMemory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryAdvanceCutoff(cpu))
            throw std::runtime_error("Cutoff native gate rejected fixture");
        std::array<uint16_t,8> nativeRegisters;
        std::copy(std::begin(cpu.r),std::end(cpu.r),nativeRegisters.begin());
        const auto nativeSR = cpu.sr, nativePC = cpu.pc;
        const auto count = cpu.native_debt+1;
        const std::vector<uint8_t> nativeMemory(std::begin(cpu.sram),std::end(cpu.sram));
        std::copy(originalMemory.begin(),originalMemory.end(),std::begin(cpu.sram));
        std::copy(originalRegisters.begin(),originalRegisters.end(),std::begin(cpu.r));
        cpu.sr = originalSR;
        cpu.pc = 0x473c;
        cpu.native_debt = 0;
        unsigned steps = 0;
        while (cpu.pc != 0x47ee && cpu.pc != 0x47f4 && cpu.pc != 0x47fa && ++steps < 1000) {
            auto opcode = MCU_ReadCodeAdvance(cpu);
            MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.sr != nativeSR || cpu.pc != nativePC
            || !std::equal(nativeRegisters.begin(),nativeRegisters.end(),std::begin(cpu.r))
            || !std::equal(nativeMemory.begin(),nativeMemory.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Cutoff %04x/%04x/%u/%u steps %u/%u SR %04x/%04x PC %04x/%04x\n",
                         control,previous,flag,selector,steps,count,cpu.sr,nativeSR,cpu.pc,nativePC);
            for (unsigned i = 0; i < 8; ++i)
                std::fprintf(stderr,"r%u=%04x/%04x ",i,cpu.r[i],nativeRegisters[i]);
            throw std::runtime_error("Cutoff conversion differs at RTS");
        }
        ++cases;
        return mcu_native::ReadWord(cpu,voice+0x24);
    };
    for (uint16_t control : {0,1,0xff,0x100,0x1234,0x7ffe,0x7fff})
        for (uint8_t flag : {0,7,8,64,127})
            for (uint16_t selector = 0; selector <= 8; ++selector) {
                const auto level = check(control,0,flag,selector);
                for (uint16_t previous : {level,uint16_t(level-1),uint16_t(level+1),uint16_t(0xffff)})
                    check(control,previous,flag,selector);
            }
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    for (unsigned i = 0; i < 1024; ++i) {
        const auto control = uint16_t(next()&0x7fff), previous = next(), flag = uint16_t(next()&127), selector = uint16_t(next()%9);
        check(control,previous,uint8_t(flag),selector);
    }
    for (unsigned guard = 0; guard < 12; ++guard) {
        cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.sr = 0x0700;
        cpu.pc = 0x473c;
        cpu.r[0] = voice;
        cpu.native_v121_enabled = true;
        MCU_Write16(cpu,voice+0x22,0);
        MCU_Write(cpu,voice+0x68,8);
        MCU_Write16(cpu,voice-0x30,1);
        switch (guard) {
            case 0: cpu.sr = 0; break;
            case 1: cpu.sr |= STATUS_T; break;
            case 2: cpu.cp = 1; break;
            case 3: cpu.dp = 1; break;
            case 4: cpu.ep = 1; break;
            case 5: cpu.r[0] = 0x9401; break;
            case 6: cpu.r[0] = 0x802e; break;
            case 7: cpu.r[0] = 0xdf98; break;
            case 8: MCU_Write16(cpu,voice+0x22,0x8000); break;
            case 9: MCU_Write(cpu,voice+0x68,128); break;
            case 10: MCU_Write16(cpu,voice-0x30,9); break;
            case 11: cpu.native_v121_enabled = false; break;
        }
        const std::vector<uint8_t> before(std::begin(cpu.sram),std::end(cpu.sram));
        std::array<uint16_t,8> registers;
        std::copy(std::begin(cpu.r),std::end(cpu.r),registers.begin());
        const auto sr = cpu.sr;
        if (mcu_native::TryAdvanceCutoff(cpu) || cpu.pc != 0x473c || cpu.native_debt != 0
            || cpu.sr != sr || !std::equal(registers.begin(),registers.end(),std::begin(cpu.r))
            || !std::equal(before.begin(),before.end(),std::begin(cpu.sram)))
            throw std::runtime_error("Native cutoff fallback mutated state");
    }
    std::printf("Cutoff: %u register/SR/SRAM/instruction-count cases and fallback guards matched\n",cases);
}
}
