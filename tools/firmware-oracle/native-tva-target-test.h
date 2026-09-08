#pragma once
#include "mcu_native.h"
#include <array>
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeTvaTarget(mcu_t& cpu, bool continuation = false)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    const uint16_t entry = continuation ? 0x3666 : 0x365d;
    auto check = [&](uint16_t previous, uint16_t target) {
        cpu.native_v121_enabled = true; cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.pc = entry; cpu.sr = uint16_t(0x0700|(cases&15)); cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        const unsigned voice = 0xacde + (cases%24)*0x12a;
        cpu.r[0] = uint16_t(voice); cpu.r[2] = target;
        MCU_Write16(cpu,voice+28,previous);
        if (continuation) { cpu.r[6] = previous; cpu.r[3] = uint16_t(cases%11); }
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const auto initialSr = cpu.sr;
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryEncodeTvaTarget(cpu)) throw std::runtime_error("TVA target encoding rejected");
        const auto sr = cpu.sr, pc = cpu.pc; const unsigned count = cpu.native_debt+1;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        cpu.pc = entry; cpu.sr = initialSr; cpu.native_debt = 0;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        unsigned steps = 0;
        while (cpu.pc != 0x36a7 && cpu.pc != 0x36ad && ++steps < 64) {
            const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.pc != pc || cpu.sr != sr
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"TVA target encoding %u/%u count=%u/%u SR=%04x/%04x\n",
                         previous,target,steps,count,cpu.sr,sr);
            throw std::runtime_error("TVA target encoding differs from H8");
        }
        ++cases;
    };
    constexpr uint16_t edges[]{0,1,2,3,15,16,17,127,128,255,256,257,32767,32768,65534,65535};
    for (auto previous : edges) for (auto target : edges) check(previous,target);
    for (unsigned i = 0; i < 4096; ++i) { const auto a = next(), b = next(); check(a,b); }
    std::printf("TVA target encoding: %u register/SR/SRAM/count cases matched\n",cases);
}
}
