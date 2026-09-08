#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeLfoPhase(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x3b26,0x3b28,0x3b2c,0x3b52,0x3b2f,0x3b55,0x3b48,0x3b32,0x3b58,0x3b34,0x3b5a,0x3b37,0x3b5d,0x3be1,0x3b3b,0x3b61,0x3b3d,0x3b63,0x3b41,0x3b67,0x3b43,0x3b69,0x3b45,0x3b6e,0x3b4b,0x3b4d,0x3b6b,0x3b71,0x3baa,0x3b74,0x3b86,0x3b99,0x3b76,0x3b7a,0x3b88,0x3b8c,0x3b9b,0x3b9f,0x3b78,0x3b7e,0x3b8a,0x3b90,0x3b9d,0x3ba3,0x3b7c,0x3b8e,0x3ba1,0x3b80,0x3bad,0x3b92,0x3bb3,0x3ba5,0x3bb9,0x3b83,0x3bb0,0x3b95,0x3bb6,0x3ba8,0x3bbc,0x3bbe,0x3bc1,0x3bc3,0x3bc7,0x3bca,0x3bcc,0x3bd7,0x3bce,0x3bd9,0x3bd1,0x3bdc,0x3bd3,0x3bd5,0x3bde,0x3be5,0x3be8,0x3bec};
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
        const auto sr = cpu.sr;
        if (entry == 0x3bc3) cpu.r[3] = uint16_t((variant%256)*2);
        if (entry == 0x3be8) cpu.r[2] = uint16_t((variant%6)*2);
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepLfoPhase(cpu) || cpu.native_debt)
            throw std::runtime_error("LFO phase rejected");
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
            std::fprintf(stderr,"LFO phase mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("LFO phase differs from H8");
        }
        ++cases;
    }
    std::printf("Native LFO phase: %u instruction boundaries matched\n",cases);
}
}
