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
inline void verifyNativeVoicePatchScale(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x4a4e,0x4a52,0x4a56,0x4a5a,0x4a5c,0x4a5f,0x4a61,0x4a64,0x4a66,0x4a69,0x4a6b,0x4a6e,0x4a71,0x4a73,0x4a76,0x4a79,0x4a7b,0x4a7e,0x4a81,0x4a83,0x4a86,0x4a88,0x4a8c,0x4a8e,0x4a91,0x4a94,0x4a96,0x4a98,0x4a9a,0x4a9d,0x4aa0,0x4aa3,0x4aa6,0x4aa9,0x4aab,0x4aaf,0x4ab3,0x4ab5,0x4ab7,0x4aba,0x4abc,0x4ac0,0x4ac2,0x4ac6,0x4aca,0x4acc,0x4ace,0x4ad1,0x4ad3,0x4ad7};
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
        if (entry == 0x4aab || entry == 0x4aaf) cpu.r[3] = uint16_t((variant%256)*2);
        if (entry == 0x4abc || entry == 0x4ad3) cpu.r[2] = uint16_t((variant%256)*2);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepVoicePatchScale(cpu) || cpu.native_debt)
            throw std::runtime_error("Second voice patch scale rejected");
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
            std::fprintf(stderr,"Second voice patch scale mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second voice patch scale differs from H8");
        }
        ++cases;
    }
    std::printf("Native voice patch scale: %u instruction boundaries matched\n",cases);
}
}
