#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeVoiceService(mcu_t& cpu)
{
    std::vector<uint16_t> entries{0x335e,0x3362};
    for (unsigned slot = 0; slot < 5; ++slot)
        for (unsigned offset : {0u,4u,5u,6u,7u,8u,12u,17u,19u})
            entries.push_back(uint16_t(0x32f0+slot*22+offset));
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
        constexpr uint16_t stages[]{0,1,12,13,14,15,22,0x7fff,0x8000,0x800d,0x800e,0xffff};
        MCU_Write16(cpu,cpu.r[0],stages[variant%12]);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepVoiceService(cpu) || cpu.native_debt)
            throw std::runtime_error("Voice service rejected");
        const auto nextPc = cpu.pc, nextSr = cpu.sr;
        const auto nextIgnore = cpu.ex_ignore;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        cpu.pc = entry; cpu.sr = sr; cpu.ex_ignore = 0;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        const auto opcode = MCU_ReadCodeAdvance(cpu);
        MCU_Operand_Table[opcode](cpu,opcode);
        if (cpu.pc != nextPc || cpu.sr != nextSr || cpu.ex_ignore != nextIgnore
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Voice service mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Voice service differs from H8");
        }
        ++cases;
    }
    std::printf("Native Voice services: %u instruction boundaries matched\n",cases);
}
}
