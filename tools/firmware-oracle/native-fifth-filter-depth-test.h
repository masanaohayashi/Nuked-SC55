#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeFifthFilterDepth(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x410d,0x4110,0x4112,0x4115,0x4119,0x411b,0x411d,0x411f,0x4121,0x4123,0x4125,0x4127,0x4129,
        0x412b,0x412e,0x4130,0x4132,0x4134,0x4136,0x413a,0x413c,0x413e,0x4140,0x4142,0x4144,
        0x415e,0x4161,0x4163,0x4165,0x4167,0x4169,0x416d,0x416f,0x4171,0x4173,0x4175,0x4177,
        0x4147,0x4149,0x414b,0x414d,0x414f,0x4151,0x4153,0x4155,0x4157,0x4159,0x415c,0x417a,0x417c,0x417e,0x4180,0x4183,0x4185,0x4187,0x4189};
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
        if (entry == 0x4136 || entry == 0x4169) cpu.r[3] = uint16_t(variant%256);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepFifthFilterDepth(cpu) || cpu.native_debt)
            throw std::runtime_error("Second fifth filter depth rejected");
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
            std::fprintf(stderr,"Second fifth filter depth mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second fifth filter depth differs from H8");
        }
        ++cases;
    }
    std::printf("Native fifth filter depth: %u instruction boundaries matched\n",cases);
}
}
