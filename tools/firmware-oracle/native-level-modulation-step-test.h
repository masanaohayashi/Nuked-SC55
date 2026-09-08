#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeLevelModulationSteps(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x312b,0x312d,0x312f,0x3131,0x3133,0x3135,0x3138,0x313a,0x313d,0x313f,0x3141,0x3145,0x3143,0x3149,0x314c,0x314e,0x3151,0x3153,0x3155,0x3157,0x3159,0x315b,0x315d,0x315f,0x3161,0x3163,0x3165,0x3167,0x3169,0x3177,0x3183,0x3147,0x316b,0x3179,0x316d,0x317b,0x316f,0x317d,0x3173,0x3181,0x3175,0x3185,0x3187};
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
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepLevelModulation(cpu) || cpu.native_debt)
            throw std::runtime_error("Level modulation step rejected");
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
            std::fprintf(stderr,"Level modulation step mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Level modulation step differs from H8");
        }
        ++cases;
    }
    std::printf("Native level modulation steps: %u instruction boundaries matched\n",cases);
}
}
