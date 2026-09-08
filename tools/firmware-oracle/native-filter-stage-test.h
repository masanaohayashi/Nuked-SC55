#pragma once
#include "mcu_native.h"
#include <array>
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeFilterStage(mcu_t& cpu)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    auto check = [&](uint16_t stage, uint16_t phase, uint8_t disabled) {
        cpu.native_v121_enabled = true; cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.pc = 0x4443; cpu.sr = uint16_t(0x0700|(cases&15)); cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        const unsigned voice = 0xacde + (cases%24)*0x12a;
        cpu.r[0] = uint16_t(voice); MCU_Write16(cpu,voice+2,stage);
        MCU_Write16(cpu,voice+10,phase); MCU_Write16(cpu,voice-2,uint16_t(cases%24));
        for (unsigned i = 0; i < 3; ++i) {
            MCU_Write(cpu,voice+75+i,uint8_t(next()));
            MCU_Write16(cpu,voice+88+2*i,next());
        }
        MCU_Write16(cpu,voice+86,next()); MCU_Write(cpu,voice+101,disabled);
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const auto initialSr = cpu.sr;
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryDispatchFilterStage(cpu)) throw std::runtime_error("Filter stage dispatch rejected");
        const auto sr = cpu.sr, pc = cpu.pc; const unsigned count = cpu.native_debt+1;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        cpu.pc = 0x4443; cpu.sr = initialSr; cpu.native_debt = 0;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        unsigned steps = 0;
        while (cpu.pc != pc && ++steps < 64) {
            const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.pc != pc || cpu.sr != sr
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Filter stage dispatch stage=%u phase=%u count=%u/%u SR=%04x/%04x\n",
                         stage,phase,steps,count,cpu.sr,sr);
            throw std::runtime_error("Filter stage dispatch differs from H8");
        }
        ++cases;
    };
    for (unsigned repeat = 0; repeat < 128; ++repeat)
        for (uint16_t stage = 0; stage <= 22; stage += 2)
            for (auto phase : {uint16_t(0),uint16_t(1),uint16_t(0xfffe),uint16_t(0xffff)}) {
                for (uint8_t disabled : {0,1,128,255}) check(stage,phase,disabled);
            }
    std::printf("Filter stage dispatch: %u register/SR/SRAM/count cases matched\n",cases);
}
}
