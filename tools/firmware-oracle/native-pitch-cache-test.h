#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativePitchCache(mcu_t& cpu, bool stepping = false)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    auto check = [&](uint32_t reference, uint8_t source, bool hit, uint16_t base, uint16_t cached) {
        cpu.native_v121_enabled = true;
        cpu.cp = cpu.dp = cpu.ep = 0; cpu.sr = uint16_t(0x0700|(next()&15));
        cpu.pc = 0x527c; cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        const unsigned voice = 0xacde + (cases%24)*0x12a;
        cpu.r[0] = uint16_t(voice);
        MCU_Write16(cpu,voice+46,0x9600); MCU_Write(cpu,0x9607,source);
        MCU_Write(cpu,voice+0xa4,uint8_t(hit ? source : source+1));
        MCU_Write16(cpu,voice+0xa6,cached);
        MCU_Write(cpu,voice+41,uint8_t(reference>>16));
        MCU_Write16(cpu,voice+62,uint16_t(reference)); MCU_Write16(cpu,0xc8b0,base);
        if (stepping) {
            cpu.ep = 1; cpu.sr = uint16_t(cases&15);
            unsigned steps = 0;
            while (cpu.pc != 0x5367) {
                const auto pc = cpu.pc, sr = cpu.sr;
                std::array<uint16_t,8> before, after;
                std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
                const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
                if (!mcu_native::TryStepPitchCache(cpu) || cpu.native_debt || ++steps > 2000)
                    throw std::runtime_error("Unmasked cache rejected");
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
                    || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
                    std::fprintf(stderr,"Unmasked cache mismatch at %04x\n",pc);
                    throw std::runtime_error("Unmasked cache differs from H8");
                }
            }
            ++cases; return;
        }
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const auto beforeSr = cpu.sr;
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryCorrectPitch(cpu)) throw std::runtime_error("Pitch cache fixture rejected");
        const auto sr = cpu.sr;
        const auto count = cpu.native_debt+1;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> nativeMemory(std::begin(cpu.sram),std::end(cpu.sram));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        cpu.sr = beforeSr; cpu.pc = 0x527c; cpu.native_debt = 0;
        unsigned steps = 0;
        while (cpu.pc != 0x5367 && ++steps < 2000) {
            const auto opcode = MCU_ReadCodeAdvance(cpu);
            MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.sr != sr || cpu.pc != 0x5367
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(nativeMemory.begin(),nativeMemory.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Pitch cache ref=%06x source=%02x hit=%d base=%04x cache=%04x steps=%u/%u SR=%04x/%04x\n",
                         reference,source,hit,base,cached,steps,count,cpu.sr,sr);
            for (unsigned i = 0; i < 8; ++i)
                std::fprintf(stderr,"r%u=%04x/%04x ",i,cpu.r[i],after[i]);
            throw std::runtime_error("Pitch cache differs from H8");
        }
        ++cases;
    };
    for (uint32_t reference : {0u,1u,68999u,69000u,80999u,81000u,92999u,93000u,200000u,0x800000u,0x813c68u,0xffffffu})
        for (unsigned source = 0; source < 256; ++source)
            for (bool hit : {false,true})
                check(reference,uint8_t(source),hit,uint16_t(source*257),uint16_t(source*397));
    for (uint16_t base : {0,1,0x7fff,0x8000,0xfffe,0xffff})
        for (uint16_t cached : {0,1,0x7fff,0x8000,0xfffe,0xffff})
            check(81000,128,true,base,cached);
    for (unsigned i = 0; i < 2048; ++i) {
        const uint32_t reference = (uint32_t(next())<<8)|(next()&255);
        const auto source = uint8_t(next()); const bool hit = (next()&1) != 0;
        const auto base = next(), cached = next();
        check(reference,source,hit,base,cached);
    }
    for (unsigned guard = 0; guard < 12; ++guard) {
        if (stepping && (guard == 3 || guard == 4)) continue;
        cpu.native_v121_enabled = true;
        cpu.cp = cpu.dp = cpu.ep = 0; cpu.sr = 0x0700;
        cpu.pc = 0x527c; cpu.native_debt = 0; cpu.r[0] = 0xacde;
        MCU_Write16(cpu,0xacde + 46,0x9600);
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
            case 10: MCU_Write16(cpu,0xacde + 46,0x7fff); break;
            case 11: MCU_Write16(cpu,0xacde + 46,0xdff9); break;
        }
        std::array<uint16_t,8> registers;
        if (stepping && guard >= 10) {
            cpu.pc = 0x5281; cpu.r[1] = guard == 10 ? 0x7fff : 0xdff9;
        }
        std::copy(std::begin(cpu.r),std::end(cpu.r),registers.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        const auto sr = cpu.sr, pc = cpu.pc;
        if ((stepping ? mcu_native::TryStepPitchCache(cpu) : mcu_native::TryCorrectPitch(cpu))
            || cpu.pc != pc || cpu.native_debt || cpu.sr != sr
            || !std::equal(registers.begin(),registers.end(),std::begin(cpu.r))
            || !std::equal(memory.begin(),memory.end(),std::begin(cpu.sram)))
            throw std::runtime_error("Pitch cache fallback mutated state");
    }
    std::printf("Pitch cache%s: %u register/SR/SRAM/instruction-count cases and guards matched\n",
                stepping ? " unmasked" : "",cases);
}
}
