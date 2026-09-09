#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeFilterMaximum(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x418c,0x418f,0x4192,0x4194,0x4197,0x419a,0x419c,0x419f,0x41a2,0x41a4,0x41a7,0x41aa,0x41ac,0x41af,0x41b2,0x41b4,0x41b7,0x41b9,0x41bd,
        0x41c0,0x41c3,0x41c6,0x41c8,0x41ca,0x41cc,0x41ce,0x41d0,0x41d2,0x41d6,0x41d8,0x41da,0x41dc,0x41de,0x41e0,0x41e2,0x41e4,0x41e6,
        0x41e8,0x41ea,0x41ec,0x41ee,0x41f0,0x41f2,0x41f4,0x41f6,0x41f9,0x41fd,0x41ff,0x4202,0x4204,0x4206,0x4208,0x420c,0x420e,0x4210,0x4214,0x4216,0x4219,0x421b,0x421d,0x4221,
        0x4224,0x4227,0x422a,0x422d,0x4230,0x4232,0x4234,0x4236,0x4238,0x423a,0x423c,0x423e,0x4240,0x4242,0x4244,0x4246,0x4248,0x424a,0x424c,0x424e,0x4250,0x4253,0x4257};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = 0; cpu.ep = uint8_t(variant%5);
        cpu.dp = 0;
        const auto oldEp = cpu.ep, oldDp = cpu.dp;
        cpu.tp = 0;
        cpu.pc = entry; cpu.sr = uint16_t((variant&15)|(((variant/16)%8)<<8)); cpu.native_debt = 0;
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.r[1] = uint16_t(variant%24); cpu.r[7] = 0xd000;
        MCU_Write(cpu,0xcaf4+cpu.r[1],uint8_t(variant));
        cpu.r[5] = uint16_t(0x9000+variant);
        if (entry == 0x41c0 || entry == 0x422a) cpu.r[2] = uint16_t(0x9000+variant);
        if (entry == 0x4208) cpu.r[3] = uint16_t((variant%128)*2);
        if (entry == 0x421d) cpu.r[3] = uint16_t(variant%256);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepFilterMaximum(cpu) || cpu.native_debt)
            throw std::runtime_error("Second filter maximum rejected");
        const auto nextPc = cpu.pc, nextSr = cpu.sr;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        const auto nextEp = cpu.ep, nextDp = cpu.dp, nextIgnore = cpu.ex_ignore;
        cpu.ep = oldEp; cpu.dp = oldDp; cpu.ex_ignore = 0;
        cpu.pc = entry; cpu.sr = sr;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        const auto opcode = MCU_ReadCodeAdvance(cpu);
        MCU_Operand_Table[opcode](cpu,opcode);
        if (cpu.pc != nextPc || cpu.sr != nextSr || cpu.ep != nextEp || cpu.dp != nextDp || cpu.ex_ignore != nextIgnore
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Second filter maximum mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second filter maximum differs from H8");
        }
        ++cases;
    }
    std::printf("Native filter maximum: %u instruction boundaries matched\n",cases);
}
}
