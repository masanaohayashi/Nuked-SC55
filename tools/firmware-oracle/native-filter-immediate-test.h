#pragma once
#include "mcu_native.h"
#include <array>
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeFilterImmediate(mcu_t& cpu)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    auto check = [&](uint16_t duration, uint16_t elapsed, uint16_t residual, uint16_t phase,
                     uint16_t start, uint16_t target) {
        const uint16_t entry = duration ? 0x4553 : 0x4494;
        cpu.native_v121_enabled = true; cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.pc = entry; cpu.sr = uint16_t(0x0700|(cases&15)); cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        const unsigned voice = 0xacde + (cases%24)*0x12a;
        cpu.r[0] = uint16_t(voice); cpu.r[6] = duration;
        MCU_Write16(cpu,0xac5a,elapsed); MCU_Write16(cpu,voice+20,residual);
        MCU_Write16(cpu,voice+10,phase); MCU_Write16(cpu,voice+84,start); MCU_Write16(cpu,voice+86,target);
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const auto initialSr = cpu.sr;
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TrySetFilterImmediate(cpu)) throw std::runtime_error("Filter immediate rejected");
        const auto sr = cpu.sr, pc = cpu.pc; const unsigned count = cpu.native_debt+1;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        cpu.pc = entry; cpu.sr = initialSr; cpu.native_debt = 0;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        unsigned steps = 0;
        while (cpu.pc != pc && ++steps < 64) {
            const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.pc != pc || cpu.sr != sr
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Filter immediate %u/%u/%u/%u count=%u/%u SR=%04x/%04x\n",
                         duration,elapsed,residual,phase,steps,count,cpu.sr,sr);
            throw std::runtime_error("Filter immediate differs from H8");
        }
        ++cases;
    };
    constexpr uint16_t edges[]{0,1,0x7fff,0x8000,0xfffe,0xffff};
    for (unsigned repeat = 0; repeat < 32; ++repeat)
        for (auto start : edges) for (auto target : edges)
            for (uint16_t kind : {0,1}) check(kind,next(),next(),next(),start,target);
    std::printf("Filter immediate: %u register/SR/SRAM/count cases matched\n",cases);
}
}
