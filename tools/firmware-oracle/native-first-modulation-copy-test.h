#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeFirstModulationCopy(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x3d8f,0x3d92,0x3d94,0x3d96,0x3d98,0x3d9a,0x3d9c,0x3d9e,0x3da1,0x3da4,0x3da6,0x3da8,0x3daa,0x3dac,0x3dae,0x3db0,0x3d1a,0x3d1d,0x3d26,0x3d1f,0x3d28,0x3d23,0x3d2c,0x3d68,0x3d2f,0x3d6b,0x3d6e,0x3d71,0x3d74,0x3d86,0x3d77,0x3d89,0x3d7a,0x3d8c,0x3d7c,0x3d7f,0x3d82,0x3d85,0x3d32,0x3d35,0x3d38,0x3d3b,0x3d3e,0x3d41,0x3d44,0x3d47,0x3d4a,0x3d4d,0x3d50,0x3d53,0x3d56,0x3d59,0x3d5c,0x3d5f,0x3d62,0x3d65};
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
        cpu.r[2] = uint16_t(0xacde + ((variant+7)%24)*0x12a);
        if (entry == 0x3d1f || entry == 0x3d28) cpu.r[1] = uint16_t((variant%24)*2);
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepFirstModulationCopy(cpu) || cpu.native_debt)
            throw std::runtime_error("First modulation copy rejected");
        const auto nextPc = cpu.pc, nextSr = cpu.sr;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        cpu.pc = entry; cpu.sr = sr;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        const auto opcode = MCU_ReadCodeAdvance(cpu);
        MCU_Operand_Table[opcode](cpu,opcode);
        if (cpu.pc != nextPc || cpu.sr != nextSr
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"First modulation copy mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("First modulation copy differs from H8");
        }
        ++cases;
    }
    std::printf("Native first modulation copy: %u instruction boundaries matched\n",cases);
}
}
