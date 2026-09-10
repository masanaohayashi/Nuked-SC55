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
inline void verifyNativeVoicePatchApply(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x4ada,0x4adc,0x4adf,0x4ae1,0x4ae5,0x4ae7,0x4ae9,0x4aeb,0x4aed,0x4aef,0x4af2,0x4af5,0x4af7,0x4afa,0x4afc,0x4afe,0x4b02,0x4b04,0x4b06,0x4b08,0x4b0a,0x4b0c,0x4b0f,0x4b12,0x4b14,0x4b17,0x4b19,0x4b1b,0x4b1d,0x4b20,0x4b23,0x4b26,0x4b28,0x4b2b,0x4b2d,0x4b31,0x4b33,0x4b35,0x4b37,0x4b39,0x4b3b,0x4b3e,0x4b41,0x4b43,0x4b46,0x4b48,0x4b4a,0x4b4e,0x4b50,0x4b52,0x4b54,0x4b56,0x4b58,0x4b5b,0x4b5e,0x4b60,0x4b63,0x4b65,0x4b67,0x4b69,0x4b6c,0x4b6f,0x4b72,0x4b74,0x4b77,0x4b79,0x4b7d,0x4b7f,0x4b81,0x4b83,0x4b85,0x4b87,0x4b8a,0x4b8d,0x4b8f,0x4b92,0x4b94,0x4b96,0x4b9a,0x4b9c,0x4b9e,0x4ba0,0x4ba2,0x4ba4,0x4ba7,0x4baa,0x4bac,0x4baf,0x4bb1,0x4bb3,0x4bb5,0x4bb8,0x4bbb,0x4bbe,0x4bc0,0x4bc3,0x4bc5,0x4bc9,0x4bcb,0x4bcd,0x4bcf,0x4bd1,0x4bd3,0x4bd6,0x4bd9,0x4bdb,0x4bde,0x4be0,0x4be2,0x4be6,0x4be8,0x4bea,0x4bec,0x4bee,0x4bf0,0x4bf3,0x4bf6,0x4bf8,0x4bfb,0x4bfd,0x4bff,0x4c01,0x4c04,0x4c07,0x4c0a,0x4c0c,0x4c0f,0x4c11,0x4c15,0x4c17,0x4c19,0x4c1b,0x4c1d,0x4c1f,0x4c22,0x4c25,0x4c27,0x4c2a,0x4c2c,0x4c2e,0x4c32,0x4c34,0x4c36,0x4c38,0x4c3a,0x4c3c,0x4c3f,0x4c42,0x4c44,0x4c47,0x4c49,0x4c4b,0x4c4d,0x4c50};
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
        const unsigned block = entry < 0x4b23 ? 0 : 1+(entry-0x4b23)/0x4c;
        const unsigned localPc = entry-block*0x4c;
        if (localPc == 0x4ae1 || localPc == 0x4afe) cpu.r[3] = uint16_t(variant%256);
        cpu.ex_ignore = 0;
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepVoicePatchApply(cpu) || cpu.native_debt)
            throw std::runtime_error("Second voice patch apply rejected");
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
            std::fprintf(stderr,"Second voice patch apply mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Second voice patch apply differs from H8");
        }
        ++cases;
    }
    std::printf("Native voice patch apply: %u instruction boundaries matched\n",cases);
}
}
