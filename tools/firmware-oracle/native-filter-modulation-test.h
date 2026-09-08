#pragma once
#include "mcu_native.h"
#include <array>
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeFilterModulation(mcu_t& cpu)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    auto check = [&](uint16_t depth1, uint16_t depth2, uint16_t base, uint16_t wave) {
        cpu.native_v121_enabled = true; cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.pc = 0x47fb; cpu.sr = uint16_t(0x0700|(cases&15)); cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        const unsigned voice = 0xacde + (cases%24)*0x12a;
        cpu.r[0] = uint16_t(voice); cpu.r[2] = depth1; cpu.r[3] = depth2; cpu.r[5] = base; cpu.r[6] = wave;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const auto initialSr = cpu.sr;
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryModulateFilter(cpu)) throw std::runtime_error("Filter modulation rejected");
        const auto sr = cpu.sr, pc = cpu.pc; const unsigned count = cpu.native_debt+1;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        cpu.pc = 0x47fb; cpu.sr = initialSr; cpu.native_debt = 0;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        unsigned steps = 0;
        while (cpu.pc != pc && ++steps < 64) {
            const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.pc != pc || cpu.sr != sr
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Filter modulation %u/%u/%u/%u count=%u/%u SR=%04x/%04x\n",
                         depth1,depth2,base,wave,steps,count,cpu.sr,sr);
            throw std::runtime_error("Filter modulation differs from H8");
        }
        ++cases;
    };
    constexpr uint16_t edges[]{0,1,0x17ff,0x1800,0x1801,255,256,32767,32768,65534,65535};
    for (auto duration : edges) for (auto elapsed : edges) for (auto residual : edges)
        for (auto phase : {uint16_t(0),uint16_t(1),uint16_t(0x7fff),uint16_t(0xfffe),uint16_t(0xffff)})
            check(duration,elapsed,residual,phase);
    for (unsigned i = 0; i < 1024; ++i) {
        const auto a = next(), b = next(), c = next(), d = next(); check(a,b,c,d);
    }
    std::printf("Filter modulation: %u register/SR/SRAM/count cases matched\n",cases);
}
}
