#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeFilterEntry(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x3e30,0x3e33,0x3e37,0x3e3b,0x3e3e,0x3e40,0x3e42,0x3e47,0x3e4a,0x3e4d,0x3e50,0x3e53,0x3e57,0x3e5b,
        0x3e5e,0x3e61,0x3e63,0x3e66,0x3e69,0x3e6c,0x3e6f,0x3e72,0x3e75,0x3e79,0x3e7b,0x3e7f,0x3e81,0x3e84,0x3e88,0x3e8a,0x3e8d,0x3e91,0x3e93,0x3e95,
        0x3e98,0x3e9a,0x3e9d,0x3ea1,0x3ea3,0x3ea5,0x3ea9,0x3eab,0x3ead,0x3eaf,0x3eb3,0x3eb5,0x3eb7,0x3eb9,0x3ebd,0x3ebf,0x3ec1,0x3ec3,0x3ec5,0x3ec7,0x3ec9,0x3ecd,0x3ecf,0x3ed1,0x3ed3,
        0x3ed6,0x3ed9,0x3edd,0x3edf,0x3ee2,0x3ee5,0x3ee8,0x3eea,0x3eec,0x3eee,0x3ef0,0x3ef4,0x3ef6,0x3ef8,0x3efa,0x3efc,0x3f00,0x3f02};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = 0; cpu.ep = uint8_t(variant%5);
        cpu.dp = (entry == 0x3e8d || entry == 0x3e91 || entry == 0x3e93 || entry == 0x3e95) ? 3 : 0;
        const auto oldEp = cpu.ep, oldDp = cpu.dp;
        cpu.tp = 0;
        cpu.pc = entry; cpu.sr = uint16_t((variant&15)|(((variant/16)%8)<<8)); cpu.native_debt = 0;
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.r[1] = uint16_t(variant%24); cpu.r[7] = 0xd000;
        MCU_Write(cpu,0xcaf4+cpu.r[1],uint8_t(variant));
        cpu.r[5] = uint16_t(0x9000+variant);
        if (entry == 0x3e8d) cpu.r[3] = uint16_t((variant%16)*2);
        if (entry == 0x3e93) cpu.r[3] &= 0xfffe;
        if (entry == 0x3eb9 || entry == 0x3ec9 || entry == 0x3ef0 || entry == 0x3efc) cpu.r[3] = uint16_t((variant%65)*2);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepFilterEntry(cpu) || cpu.native_debt)
            throw std::runtime_error("Filter entry rejected");
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
            std::fprintf(stderr,"Filter entry mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Filter entry differs from H8");
        }
        ++cases;
    }
    std::printf("Native filter entry: %u instruction boundaries matched\n",cases);
}
}
