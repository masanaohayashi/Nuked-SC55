#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeSecondFilterDepth(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x3f90,0x3f93,0x3f95,0x3f98,0x3f9c,0x3f9e,0x3fa0,0x3fa2,0x3fa4,0x3fa6,0x3fa8,0x3faa,0x3fac,
        0x3fae,0x3fb1,0x3fb3,0x3fb5,0x3fb7,0x3fb9,0x3fbd,0x3fbf,0x3fc1,0x3fc3,0x3fc5,0x3fc7,
        0x3fe1,0x3fe4,0x3fe6,0x3fe8,0x3fea,0x3fec,0x3ff0,0x3ff2,0x3ff4,0x3ff6,0x3ff8,0x3ffa,
        0x3fca,0x3fcc,0x3fce,0x3fd0,0x3fd2,0x3fd4,0x3fd6,0x3fd8,0x3fda,0x3fdc,0x3fdf,0x3ffd,0x3fff,0x4001,0x4003,0x4006,0x4008,0x400a,0x400c};
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
        if (entry == 0x3fb9 || entry == 0x3fec) cpu.r[3] = uint16_t(variant%256);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepSecondFilterDepth(cpu) || cpu.native_debt)
            throw std::runtime_error("Second second filter depth rejected");
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
            std::fprintf(stderr,"Second second filter depth mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second second filter depth differs from H8");
        }
        ++cases;
    }
    std::printf("Native second filter depth: %u instruction boundaries matched\n",cases);
}
}
