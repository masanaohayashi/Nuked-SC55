#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeLfoSine(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x3bee,0x3c31,0x3c48,0x3c5c,0x3bf1,0x3c34,0x3c4b,0x3c5f,0x3bf4,0x3bf6,0x3c4e,0x3c64,0x3bfa,0x3bfc,0x3c6c,0x3c76,0x3c92,0x3bfe,0x3c00,0x3c02,0x3c04,0x3c62,0x3c06,0x3c0c,0x3c0a,0x3c10,0x3c14,0x3c12,0x3c16,0x3c1e,0x3c18,0x3c20,0x3c1a,0x3c1c,0x3c22,0x3c24,0x3c26,0x3c29,0x3c2b,0x3c2d,0x3ca4,0x3c37,0x3c3a,0x3c3c,0x3c41,0x3c3f,0x3c44,0x3c52,0x3c54,0x3c58,0x3ca8,0x3c68,0x3c6a,0x3c6e,0x3c8a,0x3c72,0x3c8e,0x3c74,0x3c90,0x3c78,0x3c94,0x3c7a,0x3c96,0x3c7c,0x3c98,0x3c7f,0x3c9b,0x3c81,0x3c9d,0x3c83,0x3c9f,0x3c86,0x3ca2,0x3c30,0x3c47,0x3c57,0x3c5b,0x3c89,0x3ca7,0x3cab};
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
        cpu.r[1] = uint16_t(cpu.r[0]-(variant%2 ? 128 : 94));
        if (entry == 0x3c06 || entry == 0x3c0c) cpu.r[2] = uint16_t(variant%256);
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepLfoSine(cpu) || cpu.native_debt)
            throw std::runtime_error("LFO sine rejected");
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
            std::fprintf(stderr,"LFO sine mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("LFO sine differs from H8");
        }
        ++cases;
    }
    std::printf("Native LFO sine: %u instruction boundaries matched\n",cases);
}
}
