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
inline void verifyNativeVoiceOutput(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x5855,0x318c,0x3190,0x3195,0x319a,0x31a7,0x31b0,0x31ce,0x31fb,0x31b5,0x31d3,0x3200,0x31b7,0x31d5,0x3202,0x31c5,0x31e3,0x3210,0x5894,0x5898,0x5859,0x319d,0x585e,0x5860,0x5865,0x5867,0x586a,0x586c,0x5871,0x5876,0x587b,0x5880,0x588f,0x5885,0x588a,0x5888,0x586f,0x5874,0x5879,0x587e,0x5883,0x588d,0x5892,0x31a2,0x31a4,0x31a9,0x31c7,0x31f4,0x31ae,0x31cc,0x31f9,0x31b9,0x31d7,0x3204,0x31bb,0x31d9,0x3206,0x31be,0x31dc,0x3209,0x31c0,0x31de,0x31e5,0x320b,0x31c3,0x31e1,0x320e,0x31e8,0x31ea,0x31ec,0x31ee,0x31f0};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = cpu.dp = 0; cpu.ep = 1;
        cpu.br = 0xe0;
        cpu.ex_ignore = 0;
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
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepVoiceOutput(cpu) || cpu.native_debt)
            throw std::runtime_error("Voice output rejected");
        const auto nextPc = cpu.pc, nextSr = cpu.sr;
        const auto nextIgnore = cpu.ex_ignore;
        cpu.ex_ignore = 0;
        std::memcpy(pcmAfter.data(),cpu.pcm,pcmAfter.size());
        const auto dirtyAfter = cpu.pcm->sim_dirty;
        std::memcpy(cpu.pcm,pcmBefore.data(),pcmBefore.size()); cpu.pcm->sim_dirty = 0;
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        cpu.pc = entry; cpu.sr = sr;
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        const auto opcode = MCU_ReadCodeAdvance(cpu);
        MCU_Operand_Table[opcode](cpu,opcode);
        if (cpu.pc != nextPc || cpu.sr != nextSr
            || cpu.ex_ignore != nextIgnore
            || std::memcmp(cpu.pcm,pcmAfter.data(),pcmAfter.size()) != 0 || cpu.pcm->sim_dirty != dirtyAfter
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Voice output mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Voice output differs from H8");
        }
        ++cases;
    }
    std::printf("Native voice output: %u instruction boundaries matched\n",cases);
}
}
