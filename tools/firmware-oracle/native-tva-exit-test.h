#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeTvaExit(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x346b,0x346d,0x3471,0x3472,0x3474,0x348b,0x36a7,0x36ad,0x36da};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = cpu.dp = 0; cpu.ep = 1;
        cpu.pc = entry; cpu.sr = uint16_t((variant&15)|(((variant/16)%8)<<8)); cpu.native_debt = 0;
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.r[1] = uint16_t(variant%24); cpu.r[7] = 0xd000;
        if (entry == 0x346b || entry == 0x3472) {
            constexpr uint16_t stacks[]{0,1,0x7ffd,0x7ffe,0x7fff,0x8000,0xfffd,0xfffe,0xffff};
            cpu.r[7] = variant < 144 ? stacks[variant%9] : next();
        }
        MCU_Write(cpu,0xcaf4+cpu.r[1],uint8_t(variant));
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepTvaExit(cpu) || cpu.native_debt)
            throw std::runtime_error("TVA exit rejected");
        const auto nextPc = cpu.pc, nextSr = cpu.sr;
        const auto nextIgnore = cpu.ex_ignore;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        cpu.pc = entry; cpu.sr = sr; cpu.ex_ignore = 0;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        const auto opcode = MCU_ReadCodeAdvance(cpu);
        MCU_Operand_Table[opcode](cpu,opcode);
        if (cpu.pc != nextPc || cpu.sr != nextSr || cpu.ex_ignore != nextIgnore
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"TVA exit mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("TVA exit differs from H8");
        }
        ++cases;
    }
    std::printf("Native TVA exits: %u instruction boundaries matched\n",cases);
}
}
