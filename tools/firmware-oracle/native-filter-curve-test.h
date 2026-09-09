#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeFilterCurve(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x425b,0x425d,0x4261,0x4263,0x4266,0x4268,0x426b,0x426f,0x4271,0x4273,0x4276,0x4278,0x427b,0x427e,0x4280,0x4282,0x4284,0x4286,0x4288,0x428a,
        0x428d,0x428f,0x4291,0x4293,0x4297,0x4299,0x429b,0x429e,0x42a0,0x42a2,0x42a6,0x42a8,0x42aa,0x42ad,0x42af,0x42b1,0x42b5,0x42b8,0x42ba};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = 0; cpu.ep = uint8_t(variant%5);
        cpu.dp = (entry == 0x426b || entry == 0x426f || entry == 0x4271 || entry == 0x4273) ? 3 : 0;
        const auto oldEp = cpu.ep, oldDp = cpu.dp;
        cpu.tp = 0;
        cpu.pc = entry; cpu.sr = uint16_t((variant&15)|(((variant/16)%8)<<8)); cpu.native_debt = 0;
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.r[1] = uint16_t(variant%24); cpu.r[7] = 0xd000;
        MCU_Write(cpu,0xcaf4+cpu.r[1],uint8_t(variant));
        cpu.r[5] = uint16_t(0x9000+variant);
        if (entry == 0x426b) cpu.r[2] = uint16_t((variant%256)*2);
        if (entry == 0x4293 || entry == 0x42a2) cpu.r[3] = uint16_t(variant%256);
        if (entry == 0x42b1) cpu.r[3] = uint16_t((variant%256)*2);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepFilterCurve(cpu) || cpu.native_debt)
            throw std::runtime_error("Second filter curve rejected");
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
            std::fprintf(stderr,"Second filter curve mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second filter curve differs from H8");
        }
        ++cases;
    }
    std::printf("Native filter curve: %u instruction boundaries matched\n",cases);
}
}
