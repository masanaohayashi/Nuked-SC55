#pragma once
#include "mcu_native.h"
#include <array>
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace {
inline void verifyNativeLfo(mcu_t& cpu)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    for (unsigned test = 0; test < 4096; ++test)
    {
        cpu.native_v121_enabled = true;
        cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.sr = 0x070f;
        const uint16_t entry = test & 1 ? 0x3b26 : 0x3b2c;
        cpu.pc = entry;
        cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (test%24)*0x12a);
        const unsigned block = cpu.r[0]-(test & 1 ? 94 : 128);
        cpu.r[1] = uint16_t(block);
        for (unsigned i = 0; i < 34; i += 2) MCU_Write16(cpu,block+i,next());
        MCU_Write(cpu,block+12,next()&127);
        const unsigned shape = (test%7)*2;
        MCU_Write16(cpu,block+20,shape);
        constexpr uint16_t edges[]{0,1,0x7fff,0x8000,0xfffe,0xffff};
        MCU_Write16(cpu,block+24,edges[test%6]);
        MCU_Write16(cpu,block+26,edges[(test/6)%6]);
        MCU_Write16(cpu,0xac5a,test%3 ? next() : edges[(test/36)%6]);
        if (test%4 == 0) MCU_Write16(cpu,block+16,0);
        if (test%5 == 0) MCU_Write16(cpu,block+18,0);
        const auto originalSR = cpu.sr;
        std::array<uint16_t,8> originalRegisters;
        std::copy(std::begin(cpu.r),std::end(cpu.r),originalRegisters.begin());
        const std::vector<uint8_t> originalMemory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryAdvanceLfo(cpu)) throw std::runtime_error("LFO fixture rejected");
        std::array<uint16_t,8> nativeRegisters;
        std::copy(std::begin(cpu.r),std::end(cpu.r),nativeRegisters.begin());
        const auto nativeSR = cpu.sr, nativePC = cpu.pc;
        const auto count = cpu.native_debt+1;
        const std::vector<uint8_t> nativeMemory(std::begin(cpu.sram),std::end(cpu.sram));
        std::copy(originalMemory.begin(),originalMemory.end(),std::begin(cpu.sram));
        std::copy(originalRegisters.begin(),originalRegisters.end(),std::begin(cpu.r));
        cpu.sr = originalSR; cpu.pc = entry; cpu.native_debt = 0;
        const auto target = shape == 0 ? 0x3c30 : mcu_native::ReadWord(cpu,0x74c4+shape);
        unsigned steps = 0;
        while (cpu.pc != target && ++steps < 200) {
            auto opcode = MCU_ReadCodeAdvance(cpu);
            MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.sr != nativeSR || cpu.pc != nativePC
            || !std::equal(nativeRegisters.begin(),nativeRegisters.end(),std::begin(cpu.r))
            || !std::equal(nativeMemory.begin(),nativeMemory.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"LFO case %u shape %u steps %u/%u SR %04x/%04x PC %04x/%04x\n",
                         test,shape,steps,count,cpu.sr,nativeSR,cpu.pc,nativePC);
            for (unsigned i = 0; i < 8; ++i)
                std::fprintf(stderr,"r%u=%04x/%04x ",i,cpu.r[i],nativeRegisters[i]);
            throw std::runtime_error("LFO conversion differs");
        }
    }
    for (unsigned guard = 0; guard < 11; ++guard) {
        cpu.native_v121_enabled = true;
        cpu.cp = cpu.dp = cpu.ep = 0; cpu.sr = 0x0700;
        cpu.pc = 0x3b2c; cpu.native_debt = 0;
        cpu.r[0] = 0xacde; cpu.r[1] = 0xac5e;
        MCU_Write(cpu,0xac5e + 12,0); MCU_Write16(cpu,0xac5e + 20,0);
        switch (guard) {
            case 0: cpu.native_v121_enabled = false; break;
            case 1: cpu.cp = 1; break;
            case 2: cpu.dp = 1; break;
            case 3: cpu.ep = 1; break;
            case 4: cpu.sr = 0; break;
            case 5: cpu.sr |= STATUS_T; break;
            case 6: ++cpu.r[0]; break;
            case 7: ++cpu.r[1]; break;
            case 8: MCU_Write(cpu,0xac5e + 12,128); break;
            case 9: MCU_Write16(cpu,0xac5e + 20,14); break;
            case 10: MCU_Write16(cpu,0xac5e + 20,1); break;
        }
        std::array<uint16_t,8> registers;
        std::copy(std::begin(cpu.r),std::end(cpu.r),registers.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        const auto sr = cpu.sr;
        if (mcu_native::TryAdvanceLfo(cpu) || cpu.pc != 0x3b2c || cpu.native_debt
            || cpu.sr != sr || !std::equal(registers.begin(),registers.end(),std::begin(cpu.r))
            || !std::equal(memory.begin(),memory.end(),std::begin(cpu.sram)))
            throw std::runtime_error("LFO fallback mutated state");
    }
    std::printf("LFO: 4096 register/SR/SRAM/instruction-count cases and fallback guards matched\n");
}
}
