#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeSecondVoiceCurve(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x4d26,0x4d28,0x4d2c,0x4d2e,0x4d31,0x4d33,0x4d37,0x4d3b,0x4d3d,0x4d3f,0x4d43,0x4d45,0x4d48,0x4d4b,0x4d4d,0x4d4f,0x4d51,0x4d53,0x4d55,0x4d57,
        0x4d5a,0x4d5c,0x4d5e,0x4d60,0x4d64,0x4d66,0x4d68,0x4d6b,0x4d6d,0x4d6f,0x4d73,0x4d75,0x4d77,0x4d7a,0x4d7c,0x4d7e,0x4d82,0x4d85,0x4d87};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = 0; cpu.ep = uint8_t(variant%5);
        cpu.dp = (entry == 0x4d37 || entry == 0x4d3b || entry == 0x4d3d || entry == 0x4d3f) ? 3 : 0;
        const auto oldEp = cpu.ep, oldDp = cpu.dp;
        cpu.tp = 0;
        cpu.pc = entry; cpu.sr = uint16_t((variant&15)|(((variant/16)%8)<<8)); cpu.native_debt = 0;
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.r[1] = uint16_t(variant%24); cpu.r[7] = 0xd000;
        MCU_Write(cpu,0xcaf4+cpu.r[1],uint8_t(variant));
        cpu.r[5] = uint16_t(0x9000+variant);
        if (entry == 0x4d37) cpu.r[2] = uint16_t((variant%256)*2);
        if (entry == 0x4d60 || entry == 0x4d6f) cpu.r[3] = uint16_t(variant%256);
        if (entry == 0x4d7e) cpu.r[3] = uint16_t((variant%256)*2);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepSecondVoiceCurve(cpu) || cpu.native_debt)
            throw std::runtime_error("Second second voice curve rejected");
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
            std::fprintf(stderr,"Second second voice curve mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second second voice curve differs from H8");
        }
        ++cases;
    }
    std::printf("Native second voice curve: %u instruction boundaries matched\n",cases);
}
}
