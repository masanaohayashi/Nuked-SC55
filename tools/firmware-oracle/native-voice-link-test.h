#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeVoiceLink(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x3985,0x3988,0x398a,0x39bb,0x398d,0x39c0,0x39d2,0x398f,0x3993,0x3997,0x399b,0x399f,0x39a4,0x39a6,0x39ac,0x39b2,0x39aa,0x39b0,0x39b6,0x39b8,0x39be,0x39c2,0x39c6,0x39c9,0x39cc,0x39ce,0x39d0,0x39d4,0x39d8,0x39da,0x39de,0x39e1,0x39e4,0x39ec,0x39e8,0x39e9,0x39ea,0x39eb,0x39f0,0x39f3,0x39f5};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = cpu.dp = 0; cpu.ep = 1;
        cpu.pc = entry; cpu.sr = uint16_t((variant&15)|(((variant/16)%8)<<8)); cpu.native_debt = 0;
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.r[1] = uint16_t(variant%24); cpu.r[7] = 0xd000;
        MCU_Write(cpu,0xcaf4+cpu.r[1],uint8_t(variant));
        if (entry == 0x398f || entry == 0x39c2) cpu.r[1] = uint16_t((variant%24)*2);
        if (entry == 0x39d4 || entry == 0x39da) cpu.r[3] = uint16_t((variant%24)*2);
        if (entry == 0x399f || entry == 0x39a6 || entry == 0x39ac || entry == 0x39b2) cpu.r[2] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepVoiceLink(cpu) || cpu.native_debt)
            throw std::runtime_error("Voice link rejected");
        const auto nextPc = cpu.pc, nextSr = cpu.sr;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        const auto nextIgnore = cpu.ex_ignore; cpu.ex_ignore = 0;
        cpu.pc = entry; cpu.sr = sr;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        const auto opcode = MCU_ReadCodeAdvance(cpu);
        MCU_Operand_Table[opcode](cpu,opcode);
        if (cpu.pc != nextPc || cpu.sr != nextSr || cpu.ex_ignore != nextIgnore
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Voice link mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Voice link differs from H8");
        }
        ++cases;
    }
    std::printf("Native voice link: %u instruction boundaries matched\n",cases);
}
}
