#pragma once
#include "mcu_native.h"
#include <array>
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeVoiceUnlink(mcu_t& cpu)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    auto check = [&](uint16_t index, uint8_t first, uint8_t second) {
        cpu.native_v121_enabled = true; cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.pc = 0x3393; cpu.sr = uint16_t(0x0700|(cases&15)); cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        const unsigned voice = 0xacde + (cases%24)*0x12a;
        cpu.r[0] = uint16_t(voice); cpu.r[1] = index;
        for (unsigned i = 0; i < 24; ++i) {
            MCU_Write(cpu,0xcac4+i,uint8_t(next())); MCU_Write(cpu,0xcadc+i,uint8_t(next()));
            MCU_Write(cpu,0xac42+i,uint8_t(next()));
        }
        MCU_Write16(cpu,voice,next());
        MCU_Write(cpu,0xcac4+index,first); MCU_Write(cpu,0xcadc+index,second);
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const auto initialSr = cpu.sr;
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryUnlinkFinishedVoice(cpu)) throw std::runtime_error("Voice unlink rejected");
        const auto sr = cpu.sr, pc = cpu.pc; const unsigned count = cpu.native_debt+1;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        cpu.pc = 0x3393; cpu.sr = initialSr; cpu.native_debt = 0;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        unsigned steps = 0;
        while (cpu.pc != pc && ++steps < 64) {
            const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.pc != pc || cpu.sr != sr
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Voice unlink %u/%u/%u count=%u/%u SR=%04x/%04x\n",
                         index,first,second,steps,count,cpu.sr,sr);
            throw std::runtime_error("Voice unlink differs from H8");
        }
        ++cases;
    };
    for (uint16_t index = 0; index < 24; ++index)
        for (unsigned a = 0; a <= 24; ++a) for (unsigned b = 0; b <= 24; ++b)
            check(index,uint8_t(a == 24 ? 255 : a),uint8_t(b == 24 ? 255 : b));
    std::printf("Voice unlink: %u register/SR/SRAM/count cases matched\n",cases);
}
}
