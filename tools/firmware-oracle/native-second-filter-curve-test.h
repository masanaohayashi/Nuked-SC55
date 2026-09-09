#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeSecondFilterCurve(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x42bf,0x42c1,0x42c5,0x42c7,0x42ca,0x42cc,0x42cf,0x42d3,0x42d5,0x42d7,0x42da,0x42dc,0x42df,0x42e2,0x42e4,0x42e6,0x42e8,0x42ea,0x42ec,0x42ee,
        0x42f1,0x42f3,0x42f5,0x42f7,0x42fb,0x42fd,0x42ff,0x4302,0x4304,0x4306,0x430a,0x430c,0x430e,0x4311,0x4317,0x4319,0x431d,0x4320,0x4322,0x4313};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = 0; cpu.ep = uint8_t(variant%5);
        cpu.dp = (entry == 0x42cf || entry == 0x42d3 || entry == 0x42d5 || entry == 0x42d7) ? 3 : 0;
        const auto oldEp = cpu.ep, oldDp = cpu.dp;
        cpu.tp = 0;
        cpu.pc = entry; cpu.sr = uint16_t((variant&15)|(((variant/16)%8)<<8)); cpu.native_debt = 0;
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.r[1] = uint16_t(variant%24); cpu.r[7] = 0xd000;
        MCU_Write(cpu,0xcaf4+cpu.r[1],uint8_t(variant));
        cpu.r[5] = uint16_t(0x9000+variant);
        if (entry == 0x42cf) cpu.r[2] = uint16_t((variant%256)*2);
        if (entry == 0x42f7 || entry == 0x4306) cpu.r[3] = uint16_t(variant%256);
        if (entry == 0x4319) cpu.r[3] = uint16_t((variant%256)*2);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepSecondFilterCurve(cpu) || cpu.native_debt)
            throw std::runtime_error("Second second filter curve rejected");
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
            std::fprintf(stderr,"Second second filter curve mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second second filter curve differs from H8");
        }
        ++cases;
    }
    std::printf("Native second filter curve: %u instruction boundaries matched\n",cases);
}
}
