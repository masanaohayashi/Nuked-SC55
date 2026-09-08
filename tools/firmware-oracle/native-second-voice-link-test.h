#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeSecondVoiceLink(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x3a7a,0x3a7d,0x3a80,0x3aeb,0x3a83,0x3af0,0x3b02,0x3a85,0x3a89,0x3a8d,0x3a91,0x3a95,0x3a9a,0x3a9c,0x3aa2,0x3aa8,0x3aa0,0x3aa6,0x3aac,0x3aee,0x3af2,0x3af6,0x3af9,0x3afc,0x3afe,0x3b00,0x3b04,0x3b08,0x3b0a,0x3b0e,0x3b11,0x3b14,0x3b1c,0x3b18,0x3b19,0x3b1a,0x3b1b,0x3b20,0x3b23,0x3b25};
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
        if (entry == 0x3a85 || entry == 0x3af2) cpu.r[1] = uint16_t((variant%24)*2);
        if (entry == 0x3b04 || entry == 0x3b0a) cpu.r[3] = uint16_t((variant%24)*2);
        if (entry == 0x3a95 || entry == 0x3a9c || entry == 0x3aa2 || entry == 0x3aa8) cpu.r[2] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepSecondVoiceLink(cpu) || cpu.native_debt)
            throw std::runtime_error("Second second voice link rejected");
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
            std::fprintf(stderr,"Second second voice link mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second second voice link differs from H8");
        }
        ++cases;
    }
    std::printf("Native second voice link: %u instruction boundaries matched\n",cases);
}
}
