#pragma once
#include "mcu_native.h"
#include <array>
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeTvaInterpolation(mcu_t& cpu)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    auto check = [&](uint8_t start, uint8_t target, uint16_t phase, bool curve) {
        cpu.native_v121_enabled = true; cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.pc = 0x35db; cpu.sr = uint16_t(0x0700|(cases&15)); cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        const unsigned voice = 0xacde + (cases%24)*0x12a;
        cpu.r[0] = uint16_t(voice); cpu.r[3] = phase;
        MCU_Write(cpu,voice+96,start); MCU_Write(cpu,voice+97,target); MCU_Write(cpu,voice-8,curve);
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const auto initialSr = cpu.sr;
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryInterpolateTva(cpu)) throw std::runtime_error("TVA interpolation rejected");
        const auto sr = cpu.sr, pc = cpu.pc; const unsigned count = cpu.native_debt+1;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        if (!std::equal(memory.begin(),memory.end(),std::begin(cpu.sram)))
            throw std::runtime_error("TVA interpolation unexpectedly wrote SRAM");
        cpu.pc = 0x35db; cpu.sr = initialSr; cpu.native_debt = 0;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        unsigned steps = 0;
        while (cpu.pc != 0x365d && ++steps < 64) {
            const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.pc != pc || cpu.sr != sr
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(memory.begin(),memory.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"TVA interpolation %u/%u phase=%04x curve=%d count=%u/%u SR=%04x/%04x\n",
                         start,target,phase,curve,steps,count,cpu.sr,sr);
            throw std::runtime_error("TVA interpolation differs from H8");
        }
        ++cases;
    };
    for (unsigned index = 0; index < 256; ++index)
        for (unsigned fraction : {0u,1u,127u,255u})
            for (bool curve : {false,true}) {
                check(0,255,uint16_t(index*256+fraction),curve);
                check(255,0,uint16_t(index*256+fraction),curve);
                check(127,127,uint16_t(index*256+fraction),curve);
            }
    for (unsigned i = 0; i < 1024; ++i) {
        const auto a = next(), b = next(), phase = next(); check(uint8_t(a),uint8_t(b),phase,(i&1) != 0);
    }
    std::printf("TVA interpolation: %u register/SR/SRAM/count cases matched\n",cases);
}
}
