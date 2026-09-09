#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeFilterDepth(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x3f05,0x3f08,0x3f0a,0x3f0d,0x3f0f,0x3f13,0x3f16,0x3f18,0x3f1b,0x3f1f,0x3f21,0x3f23,0x3f25,0x3f27,0x3f29,0x3f2b,0x3f2d,0x3f2f,
        0x3f31,0x3f33,0x3f35,0x3f37,0x3f39,0x3f3b,0x3f3f,0x3f41,0x3f43,0x3f45,0x3f47,0x3f49,
        0x3f63,0x3f65,0x3f67,0x3f69,0x3f6b,0x3f6d,0x3f71,0x3f73,0x3f75,0x3f77,0x3f79,0x3f7b,
        0x3f4c,0x3f4e,0x3f50,0x3f52,0x3f54,0x3f56,0x3f58,0x3f5a,0x3f5c,0x3f5e,0x3f61,0x3f7e,0x3f80,0x3f82,0x3f84,0x3f87,0x3f89,0x3f8b,0x3f8d};
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
        if (entry == 0x3f0f) cpu.r[3] = uint16_t((variant%256)*2);
        if (entry == 0x3f3b || entry == 0x3f6d) cpu.r[3] = uint16_t(variant%256);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepFilterDepth(cpu) || cpu.native_debt)
            throw std::runtime_error("Filter depth rejected");
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
            std::fprintf(stderr,"Filter depth mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Filter depth differs from H8");
        }
        ++cases;
    }
    std::printf("Native filter depth: %u instruction boundaries matched\n",cases);
}
}
