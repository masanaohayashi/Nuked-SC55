#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativePitchInit(mcu_t& cpu, bool stepping = false)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    constexpr uint16_t edges[]{0,1,5999,6000,6001,0x7fff,0x8000,0x8001,0xffff};
    unsigned cases = 0;
    const uint16_t entry = 0x4f51;
    auto check = [&](uint16_t first, uint16_t second, uint16_t wave, uint32_t pitch) {
        cpu.native_v121_enabled = true;
        cpu.cp = cpu.dp = cpu.ep = 0; cpu.sr = 0x070f;
        cpu.pc = entry; cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        const unsigned voice = 0xacde + (cases%24)*0x12a;
        cpu.r[0] = uint16_t(voice);
        MCU_Write(cpu,voice-59,uint8_t(first));
        MCU_Write16(cpu,voice-2,uint16_t(second%24));
        MCU_Write(cpu,0xc8e4+second%24,uint8_t(wave%16));
        MCU_Write16(cpu,0xac5a,uint16_t(pitch));
        MCU_Write16(cpu,0xac5c,next());
        MCU_Write16(cpu,voice+12,next()); MCU_Write(cpu,voice+0xa4,uint8_t(next()));
        MCU_Write16(cpu,voice+22,next()); MCU_Write16(cpu,voice-58,next());
        MCU_Write(cpu,voice+45,uint8_t(pitch>>16));
        MCU_Write16(cpu,voice+70,uint16_t(pitch));
        MCU_Write(cpu,voice+41,uint8_t(pitch>>16));
        MCU_Write16(cpu,voice+62,uint16_t(pitch));
        if (stepping) {
            cpu.ep = 1; cpu.sr = uint16_t(cases&15); // actual unmasked startup context
            unsigned steps = 0;
            while (cpu.pc != 0x4f5c && cpu.pc != 0x4f85) {
                const auto pc = cpu.pc, sr = cpu.sr;
                std::array<uint16_t,8> before, after;
                std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
                const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
                if (!mcu_native::TryStepPitchInitialisation(cpu) || cpu.native_debt || ++steps > 14)
                    throw std::runtime_error("Unmasked init step rejected");
                const auto nextPc = cpu.pc, nextSr = cpu.sr;
                std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
                const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
                cpu.pc = pc; cpu.sr = sr;
                std::copy(before.begin(),before.end(),std::begin(cpu.r));
                std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
                const auto opcode = MCU_ReadCodeAdvance(cpu);
                MCU_Operand_Table[opcode](cpu,opcode);
                if (cpu.pc != nextPc || cpu.sr != nextSr
                    || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
                    || !std::equal(result.begin(),result.end(),std::begin(cpu.sram)))
                    throw std::runtime_error("Unmasked init differs at an instruction boundary");
            }
            ++cases; return;
        }
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryInitialisePitchEnvelope(cpu))
            throw std::runtime_error("Pitch fixture rejected");
        const auto exit = cpu.pc;
        if (exit != 0x4f5c && exit != 0x4f85) throw std::runtime_error("Unexpected init exit");
        const auto sr = cpu.sr;
        const auto count = cpu.native_debt+1;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> nativeMemory(std::begin(cpu.sram),std::end(cpu.sram));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        cpu.sr = 0x070f; cpu.pc = entry; cpu.native_debt = 0;
        unsigned steps = 0;
        while (cpu.pc != exit && ++steps < 2000) {
            auto opcode = MCU_ReadCodeAdvance(cpu);
            MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.sr != sr || cpu.pc != exit
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(nativeMemory.begin(),nativeMemory.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Pitch init %04x/%04x/%04x/%06x steps %u/%u SR %04x/%04x\n",
                         first,second,wave,pitch,steps,count,cpu.sr,sr);
            for (unsigned i = 0; i < 8; ++i)
                std::fprintf(stderr,"r%u=%04x/%04x ",i,cpu.r[i],after[i]);
            throw std::runtime_error("Pitch init differs from H8");
        }
        ++cases;
    };
    for (auto first : edges) for (auto second : edges) for (auto wave : edges)
        for (uint32_t pitch : {0u,1u,0xffffu,0x10000u,0x7fffffu,0xffffffu})
            check(first,second,wave,pitch);
    for (unsigned i = 0; i < 1024; ++i) {
        const auto first = next(), second = next(), wave = next();
        const uint32_t pitch = (uint32_t(next())<<8)|(next()&255);
        check(first,second,wave,pitch);
    }
    for (unsigned guard = 0; guard < 12; ++guard) {
        if (stepping && (guard == 3 || guard == 4 || guard >= 10)) continue;
        cpu.native_v121_enabled = true;
        cpu.cp = cpu.dp = cpu.ep = 0; cpu.sr = 0x0700;
        cpu.pc = entry; cpu.native_debt = 0; cpu.r[0] = 0xacde;
        MCU_Write(cpu,0xacde - 59,0x80); MCU_Write16(cpu,0xacde - 2,0);
        MCU_Write(cpu,0xc8e4,0);
        switch (guard) {
            case 0: cpu.native_v121_enabled = false; break;
            case 1: cpu.cp = 1; break;
            case 2: cpu.dp = 1; break;
            case 3: cpu.ep = 1; break;
            case 4: cpu.sr = 0; break;
            case 5: cpu.sr |= STATUS_T; break;
            case 6: ++cpu.r[0]; break;
            case 7: cpu.r[0] = 0x8000; break;
            case 8: cpu.r[0] = 0xc8ce; break;
            case 9: ++cpu.pc; break;
            case 10: MCU_Write16(cpu,0xacde - 2,24); break;
            case 11: MCU_Write(cpu,0xc8e4,16); break;
        }
        std::array<uint16_t,8> registers;
        std::copy(std::begin(cpu.r),std::end(cpu.r),registers.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        const auto sr = cpu.sr, pc = cpu.pc;
        if ((stepping ? mcu_native::TryStepPitchInitialisation(cpu) : mcu_native::TryInitialisePitchEnvelope(cpu))
            || cpu.pc != pc || cpu.native_debt || cpu.sr != sr
            || !std::equal(registers.begin(),registers.end(),std::begin(cpu.r))
            || !std::equal(memory.begin(),memory.end(),std::begin(cpu.sram)))
            throw std::runtime_error("Pitch fallback mutated state");
    }
    std::printf("Pitch init%s: %u register/SR/SRAM/instruction-count cases and guards matched\n",
                stepping ? " unmasked steps" : "",cases);
}
}
