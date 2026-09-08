#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeVoiceScan(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x5b0b,0x5b0e,0x5b5f,0x5b11,0x5b62,0x5b13,0x5b42,0x5b64,0x5b54,0x5b15,0x5b44,0x5b56,0x5b66,0x5b19,0x5b1d,0x5b1f,0x5b22,0x5b24,0x5b28,0x5b2c,0x5b2e,0x5b35,0x5b33,0x5b3a,0x5b3c,0x5b4a,0x5b40,0x5b48,0x5b5a,0x5b4e,0x5b50,0x5b52,0x5b5c,0x5b6d,0x5b6a,0x5b70,0x5b73,0x5b89,0x5b9c,0x5bb4,0x5bdb,0x5bf9,0x5b7e,0x5b91,0x5bc6,0x5be3,0x5b8d,0x5b8e,0x5b8f,0x5b90,0x5bdf,0x5be0,0x5be1,0x5be2,0x5b77,0x5bb8,0x5bbf,0x5b7b,0x5bbc,0x5bc3,0x5b82,0x5b95,0x5ba0,0x5ba7,0x5bca,0x5be7,0x5bfd,0x5c0b,0x5bd0,0x5bee,0x5c04,0x5c12,0x5bd4,0x5bf2,0x5bae,0x5c19,0x5bb2,0x5c1d,0x5b86,0x5b99,0x5ba4,0x5bab,0x5bce,0x5bd8,0x5beb,0x5bf6,0x5c01,0x5c08,0x5c0f,0x5c16};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = cpu.dp = 0; cpu.ep = 1;
        cpu.tp = 0;
        cpu.pc = entry; cpu.sr = uint16_t((variant&15)|(((variant/16)%8)<<8)); cpu.native_debt = 0;
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.r[1] = uint16_t(variant%24); cpu.r[7] = 0xd000;
        MCU_Write(cpu,0xcaf4+cpu.r[1],uint8_t(variant));
        if (entry == 0x5b15 || entry == 0x5b44 || entry == 0x5b66) cpu.r[2] = uint16_t((variant%24)*2);
        if (entry == 0x5b56) cpu.r[0] = uint16_t((variant%24)*2);
        cpu.ex_ignore = 0;
        if (entry == 0x5bc3) cpu.r[2] = uint16_t(0xacde + (variant%24)*0x12a);
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepVoiceScan(cpu) || cpu.native_debt)
            throw std::runtime_error("Voice scan rejected");
        const auto nextPc = cpu.pc, nextSr = cpu.sr;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        const auto nextEp = cpu.ep, nextIgnore = cpu.ex_ignore;
        cpu.ep = 1; cpu.ex_ignore = 0;
        cpu.pc = entry; cpu.sr = sr;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        const auto opcode = MCU_ReadCodeAdvance(cpu);
        MCU_Operand_Table[opcode](cpu,opcode);
        if (cpu.pc != nextPc || cpu.sr != nextSr || cpu.ep != nextEp || cpu.ex_ignore != nextIgnore
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Voice scan mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Voice scan differs from H8");
        }
        ++cases;
    }
    std::printf("Native voice scan: %u instruction boundaries matched\n",cases);
}
}
