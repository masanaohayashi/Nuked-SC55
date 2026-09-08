#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeSharedThirdDepth(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x3dbb,0x3dbf,0x3dc1,0x3dc3,0x3dc6,0x3dcd,0x3dc9,0x3dcb,0x3dd0,0x3dd2,0x3de8,0x3dd4,0x3dde,0x3dea,0x3df4,0x3dd6,0x3df6,0x3de0,0x3dec,0x3dd8,0x3de2,0x3dee,0x3df8,0x3dda,0x3dfa,0x3de4,0x3df0,0x3ddc,0x3de6,0x3df2,0x3dfc,0x3e0f,0x3dfe,0x3e11,0x3e02,0x3e04,0x3db3,0x3db5,0x3db8,0x3e07,0x3e0b,0x3e09,0x3e18,0x3e0d,0x3e15,0x3e1a,0x3e1d,0x3e1e,0x3e24,0x3e21,0x3e27,0x3e2a,0x3e2d};
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
        if (entry == 0x3db8) cpu.r[2] = uint16_t(0x9000+variant);
        if (entry == 0x3dfe || entry == 0x3e11) cpu.r[2] = uint16_t((variant%128)*2);
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepSharedThirdDepth(cpu) || cpu.native_debt)
            throw std::runtime_error("Shared third depth rejected");
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
            std::fprintf(stderr,"Shared third depth mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Shared third depth differs from H8");
        }
        ++cases;
    }
    std::printf("Native shared third depth: %u instruction boundaries matched\n",cases);
}
}
