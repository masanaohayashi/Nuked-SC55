#pragma once
#include "mcu_native.h"
#include <array>
#include <algorithm>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeFilterDuration(mcu_t& cpu)
{
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    auto check = [&](unsigned kind, uint8_t control, uint8_t base, uint16_t scale1, uint16_t scale2, uint16_t level) {
        cpu.native_v121_enabled = true; cpu.cp = cpu.dp = cpu.ep = 0;
        const uint16_t entry = kind == 0 ? 0x44a3 : kind == 1 ? 0x44ff : 0x4564;
        cpu.pc = entry; cpu.sr = uint16_t(0x0700|(cases&15)); cpu.native_debt = 0;
        for (auto& reg : cpu.r) reg = next();
        const unsigned voice = 0xacde + (cases%24)*0x12a;
        cpu.r[0] = uint16_t(voice); MCU_Write16(cpu,voice+46,0x9600); MCU_Write(cpu,0x9614+kind,control);
        MCU_Write(cpu,voice+74,base); MCU_Write(cpu,voice+162,level ? 16 : 0);
        MCU_Write16(cpu,voice-(kind == 2 ? 42 : 44),scale1);
        MCU_Write16(cpu,voice-(kind == 0 ? 40 : 38),scale2);
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const auto initialSr = cpu.sr;
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryComputeFilterDuration(cpu)) throw std::runtime_error("Filter duration rejected");
        const auto sr = cpu.sr, pc = cpu.pc; const unsigned count = cpu.native_debt+1;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        cpu.pc = entry; cpu.sr = initialSr; cpu.native_debt = 0;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        unsigned steps = 0;
        while (cpu.pc != pc && ++steps < 64) {
            const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
        }
        if (steps != count || cpu.pc != pc || cpu.sr != sr
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Filter duration kind=%u control=%u base=%u scales=%u/%u count=%u/%u SR=%04x/%04x\n",
                         kind,control,base,scale1,scale2,steps,count,cpu.sr,sr);
            throw std::runtime_error("Filter duration differs from H8");
        }
        ++cases;
    };
    constexpr uint16_t edges[]{0,1,255,256,32767,32768,65534,65535};
    for (unsigned kind = 0; kind < 3; ++kind) {
        for (unsigned control = 0; control < 128; ++control)
            for (unsigned base : {0u,1u,63u,64u,126u,127u})
                for (auto scale : edges) check(kind,uint8_t(control),uint8_t(base),scale,uint16_t(65535-scale),1);
        for (unsigned i = 0; i < 1024; ++i) {
            const auto a = next(), b = next(), c = next(), d = next();
            check(kind,uint8_t(a&127),uint8_t(b&127),c,d,i%7 ? 1 : 0);
        }
    }
    std::printf("Filter duration: %u register/SR/SRAM/count cases matched\n",cases);
}
}
