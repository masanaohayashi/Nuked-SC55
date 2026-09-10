#pragma once
#include "mcu_native.h"
#include "pcm.h"
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
inline void verifyNativeVoiceBaseDifference(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x49ad,0x49b1,0x49b3,0x49b5,0x49b7,0x49ba,0x49bd,0x49c0,0x49c3,0x49c5,0x49c8,0x49ca,0x49ce,0x49d0,0x49d3,0x49d7,0x49da,0x49dd,0x49e0,0x49e3,0x49e5,0x49e9,0x49ed,0x49f0,0x49f3,0x49f6,0x49f9,0x49fb,0x49fd,0x4a00,0x4a03,0x4a06,0x4a09,0x4a0c,0x4a0f,0x4a11,0x4a14,0x4a17,0x4a1b,0x4a1f,0x4a22,0x4a25,0x4a28,0x4a2b,0x4a2d,0x4a2f,0x4a33,0x4a35,0x4a37,0x4a3b,0x4a3e,0x4a41,0x4a44,0x4a47,0x4a4b,0x4a4c,0x4a4d};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = 0; cpu.ep = uint8_t(variant%5);
        cpu.dp = 0;
        const auto oldEp = cpu.ep, oldDp = cpu.dp;
        cpu.tp = 0;
        cpu.br = 0xe0;
        cpu.pcm->select_channel = uint8_t(variant%32);
        cpu.pcm->read_latch = next(); cpu.pcm->write_latch = next(); cpu.pcm->sim_dirty = 0;
        for (auto& channel : cpu.pcm->ram2) for (auto& word : channel) word = next();
        std::array<unsigned char,offsetof(pcm_t,eram)> pcmBefore{}, pcmAfter{};
        std::memcpy(pcmBefore.data(),cpu.pcm,pcmBefore.size());
        cpu.pc = entry; cpu.sr = uint16_t((variant&15)|(((variant/16)%8)<<8)); cpu.native_debt = 0;
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.r[1] = uint16_t(variant%24); cpu.r[7] = 0xd000;
        MCU_Write(cpu,0xcaf4+cpu.r[1],uint8_t(variant));
        cpu.r[5] = uint16_t(0x9000+variant);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepVoiceBaseDifference(cpu) || cpu.native_debt)
            throw std::runtime_error("Second voice base difference rejected");
        const auto nextPc = cpu.pc, nextSr = cpu.sr;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        const auto nextEp = cpu.ep, nextDp = cpu.dp, nextIgnore = cpu.ex_ignore;
        std::memcpy(pcmAfter.data(),cpu.pcm,pcmAfter.size());
        const auto dirtyAfter = cpu.pcm->sim_dirty;
        std::memcpy(cpu.pcm,pcmBefore.data(),pcmBefore.size()); cpu.pcm->sim_dirty = 0;
        cpu.ep = oldEp; cpu.dp = oldDp; cpu.ex_ignore = 0;
        cpu.pc = entry; cpu.sr = sr;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        const auto opcode = MCU_ReadCodeAdvance(cpu);
        MCU_Operand_Table[opcode](cpu,opcode);
        if (cpu.pc != nextPc || cpu.sr != nextSr || cpu.ep != nextEp || cpu.dp != nextDp || cpu.ex_ignore != nextIgnore
            || std::memcmp(cpu.pcm,pcmAfter.data(),pcmAfter.size()) != 0 || cpu.pcm->sim_dirty != dirtyAfter
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Second voice base difference mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second voice base difference differs from H8");
        }
        ++cases;
    }
    std::printf("Native voice base difference: %u instruction boundaries matched\n",cases);
}
}
