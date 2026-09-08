#pragma once
#include "mcu_native.h"
#include <array>
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeTvaImmediate(mcu_t& cpu)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    auto check = [&](uint16_t entry, uint16_t previous, uint8_t target, uint16_t index) {
        cpu.native_v121_enabled = true; cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.pc = entry; cpu.sr = uint16_t(0x0700|(cases&15)); cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        const unsigned voice = 0xacde + (cases%24)*0x12a;
        cpu.r[0] = uint16_t(voice); cpu.r[3] = index;
        MCU_Write16(cpu,voice+28,previous); MCU_Write16(cpu,voice+18,next());
        MCU_Write16(cpu,voice+8,next()); MCU_Write(cpu,voice+97,target);
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const auto initialSr = cpu.sr;
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TrySetTvaImmediate(cpu)) throw std::runtime_error("TVA immediate rejected");
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
            std::fprintf(stderr,"TVA immediate entry=%04x count=%u/%u SR=%04x/%04x\n",
                         entry,steps,count,cpu.sr,sr);
            throw std::runtime_error("TVA immediate differs from H8");
        }
        ++cases;
    };
    for (uint16_t entry : {0x3477,0x36ae,0x36c6})
        for (unsigned target = 0; target < 256; ++target)
            for (uint16_t index = 0; index <= 8; ++index) check(entry,next(),uint8_t(target),index);
    std::printf("TVA immediate: %u register/SR/SRAM/count cases matched\n",cases);
}
}
