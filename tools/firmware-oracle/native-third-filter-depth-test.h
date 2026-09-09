#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeThirdFilterDepth(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x400f,0x4012,0x4014,0x4017,0x401b,0x401d,0x401f,0x4021,0x4023,0x4025,0x4027,0x4029,0x402b,
        0x402d,0x4030,0x4032,0x4034,0x4036,0x4038,0x403c,0x403e,0x4040,0x4042,0x4044,0x4046,
        0x4060,0x4063,0x4065,0x4067,0x4069,0x406b,0x406f,0x4071,0x4073,0x4075,0x4077,0x4079,
        0x4049,0x404b,0x404d,0x404f,0x4051,0x4053,0x4055,0x4057,0x4059,0x405b,0x405e,0x407c,0x407e,0x4080,0x4082,0x4085,0x4087,0x4089,0x408b};
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
        if (entry == 0x4038 || entry == 0x406b) cpu.r[3] = uint16_t(variant%256);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepThirdFilterDepth(cpu) || cpu.native_debt)
            throw std::runtime_error("Second third filter depth rejected");
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
            std::fprintf(stderr,"Second third filter depth mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second third filter depth differs from H8");
        }
        ++cases;
    }
    std::printf("Native third filter depth: %u instruction boundaries matched\n",cases);
}
}
