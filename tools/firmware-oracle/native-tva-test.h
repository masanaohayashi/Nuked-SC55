#pragma once

#include "mcu_native.h"
#include "rom_loader.h"
#include "native-controller-test.h"
#include "native-cutoff-test.h"
#include "native-lfo-test.h"
#include "native-pitch-modulation-test.h"
#include "native-level-test.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace
{
inline int verifyNativeTva (const std::filesystem::path& directory)
{
    common::LoadRomsetResult roms;
    const auto error = common::LoadRomset(directory, {}, common::RomLoader::Hashing, {}, roms);
    if (error != common::LoadRomsetError{})
        throw std::runtime_error(common::ToCString(error));
    if (roms.picked_name != "mk1-v1.21")
        throw std::runtime_error("TVA oracle requires hashed mk1-v1.21 ROMs");
    Emulator emulator;
    if (!emulator.Init({}) || !emulator.LoadRoms(roms.romset, roms.romset_info))
        throw std::runtime_error("Cannot initialise emulator");
    auto& cpu = emulator.GetMCU();
    if (!cpu.native_v121_enabled)
        throw std::runtime_error("Native TVA ROM gate rejected v1.21 (unset SC55_NONATIVE)");
    constexpr uint16_t voice = 0x9400;
    auto prepare = [&](uint16_t level, uint16_t ramp, uint16_t previous) {
        cpu.native_debt = 0;
        cpu.cp = cpu.dp = cpu.ep = 0;
        cpu.sr = 0x070f;
        cpu.pc = 0x36ee;
        for (unsigned i = 0; i < 8; ++i) cpu.r[i] = uint16_t(0x1111 * i);
        cpu.r[0] = voice;
        cpu.r[5] = level;
        MCU_Write16(cpu, voice + 6, ramp);
        MCU_Write16(cpu, voice + 0x18, previous);
        MCU_Write16(cpu, voice + 0x1a, 0x1234);
    };
    unsigned cases = 0;
    auto check = [&](uint16_t level, uint16_t ramp, uint16_t previous) {
        prepare(level, ramp, previous);
        if (!mcu_native::TryAdvanceTva(cpu)) throw std::runtime_error("TVA rejected fixture");
        const auto count = cpu.native_debt + 1;
        std::array<uint16_t, 8> registers;
        std::copy(std::begin(cpu.r), std::end(cpu.r), registers.begin());
        const auto sr = cpu.sr;
        const std::vector<uint8_t> memory(std::begin(cpu.sram), std::end(cpu.sram));
        prepare(level, ramp, previous);
        unsigned steps = 0;
        while (cpu.pc != 0x3734 && ++steps < 100) {
            auto opcode = MCU_ReadCodeAdvance(cpu);
            MCU_Operand_Table[opcode](cpu, opcode);
        }
        if (steps != count || cpu.sr != sr
            || !std::equal(registers.begin(), registers.end(), std::begin(cpu.r))
            || !std::equal(memory.begin(), memory.end(), std::begin(cpu.sram))) {
            std::fprintf(stderr, "TVA mismatch level=%04x ramp=%04x previous=%04x steps=%u/%u sr=%04x/%04x\n",
                         level, ramp, previous, steps, count, cpu.sr, sr);
            throw std::runtime_error("TVA differs from H8 at 3734");
        }
        ++cases;
    };
    for (uint16_t level : {0, 1, 16, 17, 0x7fff, 0x8000, 0xffff})
        for (uint16_t ramp : {0, 1, 0xdfff, 0xe000, 0xfffe, 0xffff}) {
            const auto now = uint16_t((uint32_t(level) * std::min(0xffffu, unsigned(ramp) + 0x2000)) >> 16);
            for (int delta : {-17, -16, -1, 0, 1, 16, 17})
                check(level, ramp, uint16_t(now + delta));
        }
    uint32_t random = 0x55;
    auto next = [&] { random = random * 1664525u + 1013904223u; return uint16_t(random >> 16); };
    for (unsigned i = 0; i < 4096; ++i) {
        const auto level = next(), ramp = next(), previous = next();
        check(level, ramp, previous);
    }
    for (unsigned guard = 0; guard < 9; ++guard) {
        prepare(0x8000, 0xffff, 0);
        switch (guard) {
            case 0: cpu.sr = 0; break;
            case 1: cpu.sr |= STATUS_T; break;
            case 2: cpu.cp = 1; break;
            case 3: cpu.dp = 1; break;
            case 4: cpu.ep = 1; break;
            case 5: cpu.r[0] = 0x9401; break;
            case 6: cpu.r[0] = 0x7ffe; break;
            case 7: cpu.r[0] = 0xdff0; break;
            case 8: cpu.native_v121_enabled = false; break;
        }
        if (mcu_native::TryAdvanceTva(cpu) || cpu.pc != 0x36ee || cpu.native_debt != 0)
            throw std::runtime_error("Native TVA fallback guard failed");
    }
    auto modified = roms.romset_info;
    modified.rom_data[size_t(RomLocation::ROM1)][0] ^= 1;
    if (!emulator.LoadRoms(roms.romset, modified) || cpu.native_v121_enabled)
        throw std::runtime_error("Modified ROM was allowed into native TVA");
    if (!emulator.LoadRoms(roms.romset, roms.romset_info) || !cpu.native_v121_enabled)
        throw std::runtime_error("Reload did not re-evaluate native eligibility");
    cpu.native_debt = 10;
    emulator.Reset();
    if (cpu.native_debt != 0) throw std::runtime_error("Reset retained native debt");
    std::printf("TVA: %u register/SR/SRAM/instruction-count cases matched\n", cases);
    verifyNativeControllers(cpu);
    verifyNativeCutoff(cpu);
    verifyNativeLevel(cpu);
    verifyNativeLfo(cpu);
    verifyNativePitchModulation(cpu);
    verifyNativePitchModulation(cpu,true);

    // Real boot/MIDI/PCM path, not a direct helper invocation.
    std::array<std::vector<int32_t>, 2> audio;
    std::array<double, 2> elapsed{};
    uint64_t hits = 0;
    uint64_t controllerHits = 0, controllerInstructions = 0;
    uint64_t cutoffHits = 0, cutoffInstructions = 0;
    uint64_t levelHits = 0, levelInstructions = 0;
    uint64_t lfoHits = 0, lfoInstructions = 0;
    uint64_t pitchModHits = 0, pitchModInstructions = 0;
    uint64_t pitchConvertHits = 0, pitchConvertInstructions = 0;
    const bool profile = std::getenv("SC55_TVA_PROFILE") != nullptr;
    std::vector<uint64_t> counts(0x80000);
    for (unsigned mode = 0; mode < 2; ++mode) {
        Emulator player;
        if (!player.Init({}) || !player.LoadRoms(roms.romset, roms.romset_info))
            throw std::runtime_error("Cannot initialise playback");
        player.Reset();
        auto& mcu = player.GetMCU();
        if (mode == 0) mcu.native_v121_enabled = false;
        audio[mode].reserve(600000);
        player.SetSampleCallback([](void* data, const AudioFrame<int32_t>& frame) {
            auto& samples = *static_cast<std::vector<int32_t>*>(data);
            samples.push_back(frame.left);
            samples.push_back(frame.right);
        }, &audio[mode]);
        auto run = [&](uint64_t cycles) {
            const auto end = mcu.cycles + cycles;
            while (mcu.cycles < end) {
                if (profile && mode == 0 && mcu.cycles >= 60000000 && mcu.cp < 8)
                    ++counts[(unsigned(mcu.cp) << 16) | mcu.pc];
                const bool entry = mcu.native_debt == 0 && mcu.cp == 0 && mcu.pc == 0x36ee;
                const bool controllerEntry = mcu.native_debt == 0 && mcu.cp == 0 && mcu.pc == 0x5c20;
                const bool cutoffEntry = mcu.native_debt == 0 && mcu.cp == 0 && mcu.pc == 0x473c;
                const bool levelEntry = mcu.native_debt == 0 && mcu.cp == 0 && mcu.pc == 0x309b;
                const bool lfoEntry = mcu.native_debt == 0 && mcu.cp == 0
                    && (mcu.pc == 0x3b26 || mcu.pc == 0x3b2c);
                const bool pitchModEntry = mcu.native_debt == 0 && mcu.cp == 0 && mcu.pc == 0x5368;
                const bool pitchConvertEntry = mcu.native_debt == 0 && mcu.cp == 0 && mcu.pc == 0x51e7;
                player.Step();
                if (mode && pitchConvertEntry && mcu.pc == 0x527c && mcu.native_debt) {
                    ++pitchConvertHits;
                    pitchConvertInstructions += mcu.native_debt+1;
                }
                if (mode && pitchModEntry && mcu.pc == 0x53e4 && mcu.native_debt) {
                    ++pitchModHits;
                    pitchModInstructions += mcu.native_debt+1;
                }
                if (mode && lfoEntry && mcu.native_debt) {
                    ++lfoHits;
                    lfoInstructions += mcu.native_debt+1;
                }
                if (mode && entry && mcu.pc == 0x3734 && mcu.native_debt) ++hits;
                if (mode && controllerEntry && mcu.pc == 0x5ff4 && mcu.native_debt) {
                    ++controllerHits;
                    controllerInstructions += mcu.native_debt+1;
                }
                if (mode && cutoffEntry && mcu.native_debt
                    && (mcu.pc == 0x47ee || mcu.pc == 0x47f4 || mcu.pc == 0x47fa)) {
                    ++cutoffHits;
                    cutoffInstructions += mcu.native_debt+1;
                }
                if (mode && levelEntry && mcu.native_debt
                    && (mcu.pc == 0x30ea || mcu.pc == 0x3126 || mcu.pc == 0x312a)) {
                    ++levelHits;
                    levelInstructions += mcu.native_debt+1;
                }
            }
        };
        run(60000000);
        player.PostMIDI(uint8_t(0xc0)); player.PostMIDI(uint8_t(48));
        for (uint8_t key = 36; key < 60; ++key) {
            player.PostMIDI(uint8_t(0x90)); player.PostMIDI(key); player.PostMIDI(uint8_t(100));
        }
        const auto start = std::chrono::steady_clock::now();
        run(40000000);
        // Exercise controller producers while voices are sounding, not only
        // the all-zero default contribution rows.
        const uint8_t controllers[]{0xb0,1,90, 0xd0,110, 0xa0,48,100,
                                    0xb0,16,90, 0xb0,17,50};
        player.PostMIDI(controllers);
        for (uint8_t value : {uint8_t(0xb0), uint8_t(7), uint8_t(32)}) player.PostMIDI(value);
        run(20000000);
        for (uint8_t key = 36; key < 60; ++key) {
            player.PostMIDI(uint8_t(0x80)); player.PostMIDI(key); player.PostMIDI(uint8_t(0));
        }
        run(20000000);
        elapsed[mode] = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    if (!hits) throw std::runtime_error("Real playback never dispatched native TVA");
    if (!controllerHits) throw std::runtime_error("Real playback never dispatched native controllers");
    if (!cutoffHits) throw std::runtime_error("Real playback never dispatched native cutoff");
    if (!levelHits) throw std::runtime_error("Real playback never dispatched native level");
    if (!lfoHits) throw std::runtime_error("Real playback never dispatched native LFO");
    if (!pitchModHits) throw std::runtime_error("Real playback never dispatched native pitch modulation");
    if (!pitchConvertHits) throw std::runtime_error("Real playback never dispatched native pitch conversion");
    std::printf("Real playback pitch conversion: %llu calls, %llu instructions\n",
                (unsigned long long)pitchConvertHits,(unsigned long long)pitchConvertInstructions);
    std::printf("Real playback pitch modulation: %llu calls, %llu instructions\n",
                (unsigned long long)pitchModHits,(unsigned long long)pitchModInstructions);
    std::printf("Real playback LFO: %llu calls, %llu instructions\n",
                (unsigned long long)lfoHits,(unsigned long long)lfoInstructions);
    std::printf("Level playback: %llu calls, %llu H8 instructions replaced\n",
                (unsigned long long)levelHits,(unsigned long long)levelInstructions);
    std::printf("Cutoff playback: %llu calls, %llu H8 instructions replaced\n",
                (unsigned long long)cutoffHits,(unsigned long long)cutoffInstructions);
    std::printf("Controllers playback: %llu calls, %llu H8 instructions replaced\n",
                (unsigned long long)controllerHits, (unsigned long long)controllerInstructions);
    if (profile) {
        std::vector<std::pair<uint64_t, unsigned>> buckets;
        uint64_t total = 0;
        for (unsigned base = 0; base < counts.size(); base += 256) {
            uint64_t sum = 0;
            for (unsigned j = 0; j < 256; ++j) sum += counts[base+j];
            total += sum;
            buckets.emplace_back(sum, base);
        }
        std::sort(buckets.rbegin(), buckets.rend());
        uint64_t controllerCount = 0;
        for (unsigned pc = 0x5c20; pc < 0x5ff4; ++pc) controllerCount += counts[pc];
        std::printf("H8 controller region: %llu / %llu instructions (%.2f%%)\n",
                    (unsigned long long)controllerCount, (unsigned long long)total,
                    100.0*controllerCount/total);
        for (unsigned i = 0; i < 20; ++i)
            std::printf("H8 %06x %llu %.2f%%\n", buckets[i].second,
                        (unsigned long long)buckets[i].first, 100.0*buckets[i].first/total);
    }
    if (audio[0] != audio[1]) throw std::runtime_error("Playback PCM differs from H8");
    if (std::none_of(audio[0].begin(), audio[0].end(), [](int32_t value) { return value != 0; }))
        throw std::runtime_error("Playback was silent");
    std::printf("Playback: %zu stereo frames bit-identical, %llu native TVA hits\n",
                audio[0].size()/2, static_cast<unsigned long long>(hits));
    if (!profile)
        std::printf("Smoke timing: H8 %.6fs, native %.6fs (%.2f%%); not a host CPU benchmark\n",
                    elapsed[0], elapsed[1], 100.0*(elapsed[0]-elapsed[1])/elapsed[0]);
    return 0;
}
} // namespace
