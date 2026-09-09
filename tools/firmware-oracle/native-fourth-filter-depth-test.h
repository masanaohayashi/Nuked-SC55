#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeFourthFilterDepth(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x408e,0x4091,0x4093,0x4096,0x409a,0x409c,0x409e,0x40a0,0x40a2,0x40a4,0x40a6,0x40a8,0x40aa,
        0x40ac,0x40af,0x40b1,0x40b3,0x40b5,0x40b7,0x40bb,0x40bd,0x40bf,0x40c1,0x40c3,0x40c5,
        0x40df,0x40e2,0x40e4,0x40e6,0x40e8,0x40ea,0x40ee,0x40f0,0x40f2,0x40f4,0x40f6,0x40f8,
        0x40c8,0x40ca,0x40cc,0x40ce,0x40d0,0x40d2,0x40d4,0x40d6,0x40d8,0x40da,0x40dd,0x40fb,0x40fd,0x40ff,0x4101,0x4104,0x4106,0x4108,0x410a};
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
        if (entry == 0x40b7 || entry == 0x40ea) cpu.r[3] = uint16_t(variant%256);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepFourthFilterDepth(cpu) || cpu.native_debt)
            throw std::runtime_error("Second fourth filter depth rejected");
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
            std::fprintf(stderr,"Second fourth filter depth mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second fourth filter depth differs from H8");
        }
        ++cases;
    }
    std::printf("Native fourth filter depth: %u instruction boundaries matched\n",cases);
}
}
