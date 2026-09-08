#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeVoiceParameterBias(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x3a71,0x3a73,0x3a77,0x39f6,0x3a17,0x39f9,0x3a1a,0x39fc,0x39ff,0x3a02,0x3a04,0x3a0c,0x3a06,0x3a0e,0x3a08,0x3a15,0x3a0a,0x3a10,0x3a12,0x3a1d,0x3a21,0x3a23,0x3a25,0x3a28,0x3a2f,0x3a2b,0x3a2d,0x3a32,0x3a34,0x3a4a,0x3a36,0x3a40,0x3a4c,0x3a56,0x3a38,0x3a58,0x3a42,0x3a4e,0x3a3a,0x3a44,0x3a50,0x3a5a,0x3a3c,0x3a5c,0x3a46,0x3a52,0x3a3e,0x3a48,0x3a54,0x3a5e,0x3a68,0x3a60,0x3a6a,0x3a64,0x3a66,0x3a6e};
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
        if (entry == 0x39f9) cpu.r[3] = uint16_t(0x9000+variant);
        if (entry == 0x3a1a) cpu.r[2] = uint16_t(0x9000+variant);
        if (entry == 0x3a60 || entry == 0x3a6a) cpu.r[2] = uint16_t((variant%128)*2);
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepVoiceParameterBias(cpu) || cpu.native_debt)
            throw std::runtime_error("Voice parameter bias rejected");
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
            std::fprintf(stderr,"Voice parameter bias mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Voice parameter bias differs from H8");
        }
        ++cases;
    }
    std::printf("Native voice parameter bias: %u instruction boundaries matched\n",cases);
}
}
