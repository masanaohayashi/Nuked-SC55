#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeFilterControllerScale(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x4327,0x432a,0x432e,0x4330,0x4333,0x4336,0x4338,0x433a,0x433c,0x4340,0x4343,0x4345,0x4348,0x434a,0x434d,0x434f,0x4351,0x4355,0x4357,0x4359,0x435c,0x4360,0x4362,0x4365,0x4367,0x436a,0x436d,0x436f,0x4371,0x4374,0x4376,0x4379};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = 0; cpu.ep = uint8_t(variant%5);
        cpu.dp = 0;
        const auto oldEp = cpu.ep, oldDp = cpu.dp;
        cpu.tp = 0;
        cpu.pc = entry; cpu.sr = uint16_t((variant&15)|(((variant/16)%8)<<8)); cpu.native_debt = 0;
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.r[1] = uint16_t(variant%24); cpu.r[7] = 0xd000;
        MCU_Write(cpu,0xcaf4+cpu.r[1],uint8_t(variant));
        cpu.r[5] = uint16_t(0x9000+variant);
        if (entry == 0x433c || entry == 0x435c) cpu.r[3] = uint16_t(variant%256);
        if (entry == 0x436d && !cpu.r[4]) cpu.r[4] = 1;
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepFilterControllerScale(cpu) || cpu.native_debt)
            throw std::runtime_error("Second filter controller scale rejected");
        const auto nextPc = cpu.pc, nextSr = cpu.sr;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        const auto nextEp = cpu.ep, nextDp = cpu.dp, nextIgnore = cpu.ex_ignore;
        cpu.ep = oldEp; cpu.dp = oldDp; cpu.ex_ignore = 0;
        cpu.pc = entry; cpu.sr = sr;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        const auto opcode = MCU_ReadCodeAdvance(cpu);
        MCU_Operand_Table[opcode](cpu,opcode);
        if (cpu.pc != nextPc || cpu.sr != nextSr || cpu.ep != nextEp || cpu.dp != nextDp || cpu.ex_ignore != nextIgnore
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Second filter controller scale mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second filter controller scale differs from H8");
        }
        ++cases;
    }
    std::printf("Native filter controller scale: %u instruction boundaries matched\n",cases);
}
}
