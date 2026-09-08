#pragma once
#include "mcu_native.h"
#include <array>
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeFilterPhase(mcu_t& cpu)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    auto check = [&](uint16_t duration, uint16_t elapsed, uint16_t residual, uint16_t phase,
                     uint16_t start, uint16_t target) {
        cpu.native_v121_enabled = true; cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.pc = 0x45b6; cpu.sr = uint16_t(0x0700|(cases&15)); cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        const unsigned voice = 0xacde + (cases%24)*0x12a;
        cpu.r[0] = uint16_t(voice); cpu.r[6] = duration;
        MCU_Write16(cpu,0xac5a,elapsed); MCU_Write16(cpu,voice+20,residual);
        MCU_Write16(cpu,voice+10,phase); MCU_Write16(cpu,voice+84,start); MCU_Write16(cpu,voice+86,target);
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const auto initialSr = cpu.sr;
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryAdvanceFilterPhase(cpu)) throw std::runtime_error("Filter phase advance rejected");
        const auto sr = cpu.sr, pc = cpu.pc; const unsigned count = cpu.native_debt+1;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        cpu.pc = 0x45b6; cpu.sr = initialSr; cpu.native_debt = 0;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        unsigned steps = 0;
        while (cpu.pc != pc && ++steps < 64) {
            const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.pc != pc || cpu.sr != sr
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Filter phase advance %u/%u/%u/%u count=%u/%u SR=%04x/%04x\n",
                         duration,elapsed,residual,phase,steps,count,cpu.sr,sr);
            throw std::runtime_error("Filter phase advance differs from H8");
        }
        ++cases;
    };
    constexpr uint16_t edges[]{0,1,7,8,9,10,255,256,32767,32768,65534,65535};
    for (auto duration : edges) for (auto elapsed : edges) for (auto residual : edges)
        for (auto phase : {uint16_t(0),uint16_t(1),uint16_t(0x7fff),uint16_t(0xfffe),uint16_t(0xffff)})
            check(duration,elapsed,residual,phase,next(),next());
    for (unsigned i = 0; i < 1024; ++i) {
        const auto a = next(), b = next(), c = next(), d = next(); check(a,b,c,d,next(),next());
    }
    constexpr uint16_t signedEdges[]{0,1,0x3fff,0x4000,0x7fff,0x8000,0x8001,0xffff};
    for (auto start : signedEdges) for (auto target : signedEdges)
        for (uint16_t phase : {0,0x8000,0xffff}) check(65535,0,0,phase,start,target);
    std::printf("Filter phase advance: %u register/SR/SRAM/count cases matched\n",cases);
}
}
