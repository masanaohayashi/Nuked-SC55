#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeVoiceBaseValue(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x4858,0x485c,0x485f,0x4863,0x4866,0x486a,0x486c,0x4870,0x4874,0x4876,0x4878,0x487c,0x487f,0x4882,0x4885,0x4889,
        0x488d,0x488f,0x4892,0x4896,0x4899,0x489d,0x489f,0x48a1,0x48a3,0x48a6,0x48a8,0x48aa,0x48ad,0x48b0,0x48b3,0x48b6,0x48ba,0x48bc,0x48be,0x48c0,0x48c3,0x48c5,0x48c7,0x48ca,0x48cd,0x48d0,0x48d4,
        0x48d8,0x48da,0x48dd,0x48df,0x48e1,0x48e5,0x48e7,0x48e9,0x48ed,0x48f1,0x48f3,0x48f5,0x48f9,0x48fc,
        0x48ff,0x4903,0x4905,0x4907,0x4909,0x490c,0x490e,0x4910,0x4912,0x4914,0x4916,0x4919,0x491c,0x491f,0x4923};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = 0; cpu.ep = uint8_t(variant%5);
        cpu.dp = (entry == 0x48ed || entry == 0x48f1 || entry == 0x48f3 || entry == 0x48f5) ? 3 : 0;
        const auto oldEp = cpu.ep, oldDp = cpu.dp;
        cpu.tp = 0;
        cpu.pc = entry; cpu.sr = uint16_t((variant&15)|(((variant/16)%8)<<8)); cpu.native_debt = 0;
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.r[1] = uint16_t(variant%24); cpu.r[7] = 0xd000;
        MCU_Write(cpu,0xcaf4+cpu.r[1],uint8_t(variant));
        cpu.r[5] = uint16_t(0x9000+variant);
        if (entry == 0x4896 || entry == 0x48b3) cpu.r[5] &= 0xfffe;
        if (entry == 0x4878) cpu.r[2] = uint16_t((variant%24)*2);
        if (entry == 0x48ed) cpu.r[3] = uint16_t((variant%256)*2);
        if (entry == 0x48f3) cpu.r[3] &= 0xfffe;
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepVoiceBaseValue(cpu) || cpu.native_debt)
            throw std::runtime_error("Second voice base value rejected");
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
            std::fprintf(stderr,"Second voice base value mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second voice base value differs from H8");
        }
        ++cases;
    }
    std::printf("Native voice base value: %u instruction boundaries matched\n",cases);
}
}
