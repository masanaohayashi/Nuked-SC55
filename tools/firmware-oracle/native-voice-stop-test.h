#pragma once
#include "mcu_native.h"
#include <algorithm>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
bool captureVoiceStop = false;
std::vector<std::array<uint64_t,5>> voiceStopWrites;
inline void captureVoiceStopWrite(const mcu_t& cpu, uint32_t address, uint8_t value)
{
    if (captureVoiceStop) voiceStopWrites.push_back({cpu.cycles,cpu.cp,cpu.pc,address,value});
}
inline void verifyNativeVoiceStop(mcu_t& cpu)
{
    constexpr uint16_t entries[]{0x33c9,0x33cb,0x33d0,0x33d4,0x33d8,0x33da,0x33dc};
    uint32_t random = 55;
    auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
    unsigned cases = 0;
    for (auto entry : entries) for (unsigned variant = 0; variant < 384; ++variant) {
        cpu.native_v121_enabled = true;
        cpu.cp = cpu.dp = 0; cpu.ep = 1;
        cpu.pc = entry; cpu.sr = uint16_t((variant&15)|(((variant/16)%8)<<8)); cpu.native_debt = 0;
        for (auto& byte : cpu.sram) byte = uint8_t(next());
        for (auto& reg : cpu.r) reg = next();
        cpu.r[0] = uint16_t(0xacde + (variant%24)*0x12a);
        cpu.r[1] = uint16_t(variant%24); cpu.r[7] = 0xd000;
        MCU_Write(cpu,0xcaf4+cpu.r[1],uint8_t(variant));
        cpu.ex_ignore = 0; cpu.br = 0xe0; cpu.trapa_pending = {};
        captureVoiceStop = true; voiceStopWrites.clear();
        const auto sr = cpu.sr;
        std::array<uint16_t,8> before, after;
        std::copy(std::begin(cpu.r),std::end(cpu.r),before.begin());
        const std::vector<uint8_t> memory(std::begin(cpu.sram),std::end(cpu.sram));
        if (!mcu_native::TryStepVoiceStop(cpu) || cpu.native_debt)
            throw std::runtime_error("Voice stop rejected");
        const auto nextPc = cpu.pc, nextSr = cpu.sr;
        const auto nextIgnore = cpu.ex_ignore;
        const auto expectedWrites = voiceStopWrites;
        if (expectedWrites.size() != (entry == 0x33c9 ? 1u : entry == 0x33cb ? 2u : 0u))
            throw std::runtime_error("Voice stop PCM writes were not captured");
        const bool expectedTrap = cpu.trapa_pending.Contains(2);
        std::copy(std::begin(cpu.r),std::end(cpu.r),after.begin());
        const std::vector<uint8_t> result(std::begin(cpu.sram),std::end(cpu.sram));
        cpu.pc = entry; cpu.sr = sr; cpu.ex_ignore = 0; cpu.trapa_pending = {};
        voiceStopWrites.clear();
        std::copy(before.begin(),before.end(),std::begin(cpu.r));
        std::copy(memory.begin(),memory.end(),std::begin(cpu.sram));
        const auto opcode = MCU_ReadCodeAdvance(cpu);
        MCU_Operand_Table[opcode](cpu,opcode);
        if (voiceStopWrites != expectedWrites || cpu.trapa_pending.Contains(2) != expectedTrap
            || cpu.pc != nextPc || cpu.sr != nextSr || cpu.ex_ignore != nextIgnore
            || !std::equal(after.begin(),after.end(),std::begin(cpu.r))
            || !std::equal(result.begin(),result.end(),std::begin(cpu.sram))) {
            std::fprintf(stderr,"Voice stop mismatch at %04x variant %u\n",entry,variant);
            throw std::runtime_error("Voice stop differs from H8");
        }
        captureVoiceStop = false;
        ++cases;
    }
    std::printf("Native Voice stops: %u instruction boundaries matched\n",cases);
}
}
