#pragma once
#include "mcu_native.h"
#include <array>
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeVoiceRelease(mcu_t& cpu)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    auto check = [&](uint16_t stage, uint8_t request, uint16_t delay) {
        cpu.native_v121_enabled = true; cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.pc = 0x3212; cpu.sr = uint16_t(0x0700|(cases&15)); cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        const unsigned voice = 0xacde + (cases%24)*0x12a;
        cpu.r[0] = uint16_t(voice); MCU_Write16(cpu,voice-2,uint16_t(cases%24));
        for (int offset = -112; offset <= 134; ++offset) MCU_Write(cpu,voice+offset,uint8_t(next()));
        MCU_Write16(cpu,voice-2,uint16_t(cases%24));
        MCU_Write16(cpu,voice,stage); MCU_Write(cpu,0xac2a+cases%24,request);
        MCU_Write16(cpu,voice+16,delay);
        if (cases%3 == 0) MCU_Write16(cpu,voice-78,0);
        if (cases%5 == 0) MCU_Write16(cpu,voice-112,0);
        if (cases%7 == 0) MCU_Write(cpu,voice+111,MCU_Read(cpu,voice+44));
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const auto initialSr = cpu.sr;
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryPrepareVoiceRelease(cpu)) throw std::runtime_error("Voice release rejected");
        const auto sr = cpu.sr, pc = cpu.pc; const unsigned count = cpu.native_debt+1;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        cpu.pc = 0x3212; cpu.sr = initialSr; cpu.native_debt = 0;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        unsigned steps = 0;
        while (cpu.pc != pc && ++steps < 64) {
            const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.pc != pc || cpu.sr != sr
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Voice release %u/%u/%u count=%u/%u SR=%04x/%04x\n",
                         stage,request,delay,steps,count,cpu.sr,sr);
            throw std::runtime_error("Voice release differs from H8");
        }
        ++cases;
    };
    for (unsigned repeat = 0; repeat < 128; ++repeat)
        for (uint16_t stage : {0,1,2,10,11,12,13,14,22,0x7fff,0x8000,0xffff})
            for (uint8_t request : {0,1,255})
                for (uint16_t delay : {0,1,0xffff}) check(stage,request,delay);
    std::printf("Voice release: %u register/SR/SRAM/count cases matched\n",cases);
}
}
