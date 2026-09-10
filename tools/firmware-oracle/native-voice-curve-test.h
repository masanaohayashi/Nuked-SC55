#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeVoiceCurve(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x4cbb,0x4cbd,0x4cc0,0x4cc4,0x4cc7,0x4ccb,0x4ccd,0x4cd1,0x4cd5,0x4cd7,0x4cd9,0x4cdd,0x4cdf,
        0x4ce2,0x4ce5,0x4ce7,0x4ce9,0x4ceb,0x4ced,0x4cef,0x4cf1,0x4cf4,0x4cf6,0x4cf8,0x4cfa,0x4cfe,0x4d00,0x4d02,0x4d05,0x4d07,0x4d09,0x4d0d,0x4d0f,0x4d11,0x4d14,0x4d16,0x4d18,0x4d1c,0x4d1f,0x4d21};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = 0; cpu.ep = uint8_t(variant%5);
        cpu.dp = (entry == 0x4cd1 || entry == 0x4cd5 || entry == 0x4cd7 || entry == 0x4cd9) ? 3 : 0;
        const auto oldEp = cpu.ep, oldDp = cpu.dp;
        cpu.tp = 0;
        cpu.pc = entry; cpu.sr = uint16_t((variant&15)|(((variant/16)%8)<<8)); cpu.native_debt = 0;
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.r[1] = uint16_t(variant%24); cpu.r[7] = 0xd000;
        MCU_Write(cpu,0xcaf4+cpu.r[1],uint8_t(variant));
        cpu.r[5] = uint16_t(0x9000+variant);
        if (entry == 0x4cd1) cpu.r[3] = uint16_t((variant%16)*2);
        if (entry == 0x4cfa || entry == 0x4d09) cpu.r[3] = uint16_t(variant%256);
        if (entry == 0x4d18) cpu.r[3] = uint16_t((variant%256)*2);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepVoiceCurve(cpu) || cpu.native_debt)
            throw std::runtime_error("Second voice curve rejected");
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
            std::fprintf(stderr,"Second voice curve mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second voice curve differs from H8");
        }
        ++cases;
    }
    std::printf("Native voice curve: %u instruction boundaries matched\n",cases);
}
}
