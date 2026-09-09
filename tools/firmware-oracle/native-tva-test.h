#pragma once

#include "mcu_native.h"
#include "rom_loader.h"
#include "native-controller-test.h"
#include "native-cutoff-test.h"
#include "native-lfo-test.h"
#include "native-pitch-modulation-test.h"
#include "native-pitch-cache-test.h"
#include "native-pitch-glide-test.h"
#include "native-pitch-envelope-test.h"
#include "native-pitch-stage-test.h"
#include "native-pitch-init-test.h"
#include "native-pitch-adjust-test.h"
#include "native-pitch-connections-test.h"
#include "native-level-test.h"
#include "native-tva-interpolation-test.h"
#include "native-tva-target-test.h"
#include "native-tva-phase-test.h"
#include "native-tva-duration-test.h"
#include "native-tva-stage-test.h"
#include "native-tva-immediate-test.h"
#include "native-tva-delay-test.h"
#include "native-tva-exit-test.h"
#include "native-voice-unlink-test.h"
#include "native-voice-stop-test.h"
#include "native-voice-service-test.h"
#include "native-voice-release-test.h"
#include "native-filter-stage-test.h"
#include "native-filter-duration-test.h"
#include "native-filter-phase-test.h"
#include "native-filter-base-test.h"
#include "native-filter-modulation-test.h"
#include "native-filter-resonance-test.h"
#include "native-filter-connections-test.h"
#include "native-filter-immediate-test.h"
#include "native-filter-modulation-step-test.h"
#include "native-filter-conversion-step-test.h"
#include "native-level-modulation-test.h"
#include "native-level-modulation-step-test.h"
#include "native-level-connections-test.h"
#include "native-voice-output-test.h"
#include "native-voice-scan-test.h"
#include "native-voice-link-test.h"
#include "native-lfo-phase-test.h"
#include "native-lfo-sine-test.h"
#include "native-lfo-sample-hold-test.h"
#include "native-second-voice-link-test.h"
#include "native-voice-parameter-bias-test.h"
#include "native-shared-modulation-copy-test.h"
#include "native-first-modulation-copy-test.h"
#include "native-shared-third-depth-test.h"
#include "native-filter-entry-test.h"
#include "native-filter-depth-test.h"
#include "native-second-filter-depth-test.h"
#include "native-third-filter-depth-test.h"
#include "native-fourth-filter-depth-test.h"
#include "native-fifth-filter-depth-test.h"
#include "native-filter-maximum-test.h"
#include "native-filter-curve-test.h"
#include "native-second-filter-curve-test.h"
#include "native-filter-controller-scale-test.h"
#include "native-second-filter-controller-scale-test.h"
#include "native-filter-setup-test.h"
#include "native-voice-base-value-test.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace {
bool collectH8Fallback = false;
std::array<uint64_t,0x80000> h8FallbackCounts{};
std::array<uint64_t,8> filterLfoFallbackMasks{};
}
void Oracle_H8Fallback(const mcu_t& cpu)
{
    if (collectH8Fallback && cpu.cycles >= 60000000 && cpu.cp < 8) {
        ++h8FallbackCounts[(unsigned(cpu.cp)<<16)|cpu.pc];
        if (!cpu.cp && cpu.pc == 0x47fb) ++filterLfoFallbackMasks[(cpu.sr>>8)&7];
    }
}

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
    verifyNativePitchModulation(cpu,false,true);
    verifyNativePitchModulation(cpu,true,true);
    verifyNativePitchCache(cpu);
    verifyNativePitchCache(cpu,true);
    verifyNativePitchGlide(cpu);
    verifyNativePitchGlide(cpu,true);
    verifyNativePitchEnvelope(cpu);
    verifyNativePitchEnvelope(cpu,true);
    verifyNativePitchStages(cpu);
    verifyNativePitchStages(cpu,true);
    verifyNativePitchStages(cpu,false,true);
    verifyNativePitchStages(cpu,true,true);
    verifyNativePitchInit(cpu);
    verifyNativePitchInit(cpu,true);
    verifyNativePitchAdjust(cpu);
    verifyNativePitchAdjust(cpu,true);
    verifyNativePitchAdjust(cpu,false,true);
    verifyNativePitchAdjust(cpu,true,true);
    verifyNativePitchConnections(cpu);
    verifyNativeTvaInterpolation(cpu);
    verifyNativeTvaTarget(cpu);
    verifyNativeTvaTarget(cpu,true);
    verifyNativeTvaPhase(cpu);
    verifyNativeTvaDuration(cpu);
    verifyNativeTvaStage(cpu);
    verifyNativeTvaImmediate(cpu);
    verifyNativeTvaDelay(cpu);
    verifyNativeTvaExit(cpu);
    verifyNativeVoiceUnlink(cpu);
    verifyNativeVoiceStop(cpu);
    verifyNativeVoiceService(cpu);
    verifyNativeVoiceRelease(cpu);
    verifyNativeFilterStage(cpu);
    verifyNativeFilterDuration(cpu);
    verifyNativeFilterPhase(cpu);
    verifyNativeFilterBase(cpu);
    verifyNativeFilterModulation(cpu);
    verifyNativeFilterResonance(cpu);
    verifyNativeFilterConnections(cpu);
    verifyNativeFilterImmediate(cpu);
    verifyNativeFilterModulationSteps(cpu);
    verifyNativeFilterConversionSteps(cpu);
    verifyNativeLevelModulation(cpu);
    verifyNativeLevelModulationSteps(cpu);
    verifyNativeLevelConnections(cpu);
    verifyNativeVoiceOutput(cpu);
    verifyNativeVoiceScan(cpu);
    verifyNativeVoiceLink(cpu);
    verifyNativeLfoPhase(cpu);
    verifyNativeLfoSine(cpu);
    verifyNativeLfoSampleHold(cpu);
    verifyNativeSecondVoiceLink(cpu);
    verifyNativeVoiceParameterBias(cpu);
    verifyNativeSharedModulationCopy(cpu);
    verifyNativeFirstModulationCopy(cpu);
    verifyNativeSharedThirdDepth(cpu);
    verifyNativeFilterEntry(cpu);
    verifyNativeFilterDepth(cpu);
    verifyNativeSecondFilterDepth(cpu);
    verifyNativeThirdFilterDepth(cpu);
    verifyNativeFourthFilterDepth(cpu);
    verifyNativeFifthFilterDepth(cpu);
    verifyNativeFilterMaximum(cpu);
    verifyNativeFilterCurve(cpu);
    verifyNativeSecondFilterCurve(cpu);
    verifyNativeFilterControllerScale(cpu);
    verifyNativeSecondFilterControllerScale(cpu);
    verifyNativeFilterSetup(cpu);
    verifyNativeVoiceBaseValue(cpu);

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
    uint64_t pitchCacheHits = 0;
    uint64_t glideHits = 0, glideInstructions = 0, movingGlideHits = 0;
    uint64_t pitchEnvelopeHits = 0;
    uint64_t pitchEnvelopeStepEntries = 0;
    uint64_t pitchStageHits = 0, pitchStageInstructions = 0;
    uint64_t pitchInitHits = 0, pitchReentryHits = 0, pitchReentryVisits = 0;
    uint64_t pitchAdjustHits = 0, pitchTuningHits = 0;
    uint64_t pitchAdjustStepHits = 0, pitchTuningStepHits = 0;
    const bool profile = std::getenv("SC55_TVA_PROFILE") != nullptr;
    std::vector<uint64_t> counts(0x80000);
    for (unsigned mode = 0; mode < 2; ++mode) {
        collectH8Fallback = profile && mode == 1;
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
                const bool glideEntry = mcu.native_debt == 0 && mcu.cp == 0 && mcu.pc == 0x5175;
                const bool pitchInitEntry = mcu.native_debt == 0 && mcu.cp == 0 && mcu.pc == 0x4f51;
                const bool pitchEnvelopeStepEntry = mcu.native_debt == 0 && mcu.cp == 0 && mcu.pc == 0x5060
                    && mcu.native_v121_enabled && mcu.dp == 0 && !(mcu.sr&STATUS_T)
                    && mcu.r[0] >= 0xacde && mcu.r[0] <= 0xc7a4 && (mcu.r[0]-0xacde)%0x12a == 0;
                const bool pitchAdjustEntry = mcu.native_debt == 0 && mcu.cp == 0 && mcu.pc == 0x50cf;
                const bool pitchTuningEntry = mcu.native_debt == 0 && mcu.cp == 0 && mcu.pc == 0x5124;
                const bool pitchAdjustmentStepEligible = (pitchAdjustEntry || pitchTuningEntry)
                    && mcu.native_v121_enabled && mcu.dp == 0 && !(mcu.sr&STATUS_T)
                    && mcu.r[0] >= 0xacde && mcu.r[0] <= 0xc7a4 && (mcu.r[0]-0xacde)%0x12a == 0;
                const bool pitchReentryEntry = mcu.native_debt == 0 && mcu.cp == 0 && mcu.pc == 0x4f9e;
                if (mode && pitchReentryEntry) ++pitchReentryVisits;
                const bool pitchStageEntry = mcu.native_debt == 0 && mcu.cp == 0
                    && (mcu.pc == 0x4fdb || mcu.pc == 0x4f9e);
                bool pitchEnvelopeEntry = false;
                if (pitchStageEntry) {
                    unsigned stage = mcu_native::ReadWord(mcu,mcu.r[0]+4);
                    if (stage <= 22 && !(stage&1)) {
                        if (!pitchReentryEntry && mcu_native::ReadWord(mcu,mcu.r[0]+12) == 0xffff)
                            stage = mcu_native::ReadWord(mcu,0x6ac8+stage);
                        pitchEnvelopeEntry = mcu_native::ReadWord(mcu,0x7b62+stage) == 0x5060;
                    }
                }
                const bool movingGlide = glideEntry && (MCU_Read(mcu,mcu.r[0]+43)
                    || mcu_native::ReadWord(mcu,mcu.r[0]+66));
                player.Step();
                if (mode && pitchEnvelopeStepEntry && mcu.pc == 0x5064 && !mcu.native_debt)
                    ++pitchEnvelopeStepEntries;
                if (mode && pitchAdjustEntry && mcu.pc == 0x510a && mcu.native_debt) ++pitchAdjustHits;
                if (mode && pitchAdjustmentStepEligible && !mcu.native_debt) {
                    if (pitchAdjustEntry && mcu.pc == 0x50d2) ++pitchAdjustStepHits;
                    if (pitchTuningEntry && mcu.pc == 0x5127) ++pitchTuningStepHits;
                }
                if (mode && pitchTuningEntry && mcu.pc == 0x5175 && mcu.native_debt) ++pitchTuningHits;
                if (mode && pitchInitEntry
                    && (mcu.pc == 0x4f54 || ((mcu.pc == 0x4f5c || mcu.pc == 0x4f85) && mcu.native_debt))) ++pitchInitHits;
                if (mode && pitchReentryEntry && (mcu.native_debt
                    || (mcu.pc == 0x4fa1 && mcu.native_v121_enabled && mcu.dp == 0
                        && !(mcu.sr&STATUS_T) && mcu.r[0] >= 0xacde && mcu.r[0] <= 0xc7a4
                        && (mcu.r[0]-0xacde)%0x12a == 0))) ++pitchReentryHits;
                if (mode && pitchStageEntry && mcu.native_debt
                    && (mcu.pc == 0x5060 || mcu.pc == 0x50cf || mcu.pc == 0x5367)) {
                    ++pitchStageHits; pitchStageInstructions += mcu.native_debt+1;
                }
                if (mode && pitchEnvelopeEntry && mcu.pc == 0x50cf && mcu.native_debt) {
                    ++pitchEnvelopeHits;
                }
                if (mode && glideEntry && mcu.pc == 0x51e7 && mcu.native_debt) {
                    ++glideHits; glideInstructions += mcu.native_debt+1;
                    if (movingGlide) ++movingGlideHits;
                }
                if (mode && pitchConvertEntry && mcu.pc == 0x5367 && mcu.native_debt) {
                    ++pitchCacheHits;
                }
                if (mode && pitchConvertEntry && (mcu.pc == 0x527c || mcu.pc == 0x5367) && mcu.native_debt) {
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
        const uint8_t portamento[]{0xb0,65,127, 0xb0,5,80, 0x90,72,100};
        player.PostMIDI(portamento);
        run(20000000);
        const uint8_t glideOff[]{0x80,72,0, 0xb0,65,0};
        player.PostMIDI(glideOff);
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
    if (!pitchCacheHits) throw std::runtime_error("Real playback never dispatched native pitch cache");
    if (!glideHits) throw std::runtime_error("Real playback never dispatched native pitch glide");
    if (!pitchEnvelopeHits) throw std::runtime_error("Real playback never dispatched native pitch envelope");
    if (!pitchEnvelopeStepEntries) throw std::runtime_error("Real playback never dispatched unmasked pitch envelope");
    std::printf("Real playback unmasked pitch envelope: %llu entries\n",
                (unsigned long long)pitchEnvelopeStepEntries);
    if (!pitchStageHits) throw std::runtime_error("Real playback never dispatched native pitch stages");
    std::printf("Real playback pitch init/reentry: %llu/%llu calls\n",
                (unsigned long long)pitchInitHits,(unsigned long long)pitchReentryHits);
    if (!pitchInitHits) throw std::runtime_error("Real playback never dispatched pitch init");
    std::printf("Real playback reentry visits: %llu\n",(unsigned long long)pitchReentryVisits);
    if (!pitchAdjustHits || !pitchTuningHits)
        throw std::runtime_error("Real playback never dispatched pitch adjustment/tuning");
    if (!pitchAdjustStepHits || !pitchTuningStepHits)
        throw std::runtime_error("Real playback never dispatched unmasked pitch adjustment/tuning");
    std::printf("Real playback unmasked adjustment/tuning: %llu/%llu entries\n",
                (unsigned long long)pitchAdjustStepHits,(unsigned long long)pitchTuningStepHits);
    std::printf("Real playback pitch adjustment/tuning: %llu/%llu calls\n",
                (unsigned long long)pitchAdjustHits,(unsigned long long)pitchTuningHits);
    std::printf("Real playback pitch stages: %llu calls, %llu instructions\n",
                (unsigned long long)pitchStageHits,(unsigned long long)pitchStageInstructions);
    std::printf("Real playback pitch envelope: %llu composed calls\n",
                (unsigned long long)pitchEnvelopeHits);
    std::printf("Real playback pitch glide: %llu calls, %llu instructions\n",
                (unsigned long long)glideHits,(unsigned long long)glideInstructions);
    if (!movingGlideHits) throw std::runtime_error("Real playback never dispatched a nonzero glide");
    std::printf("Nonzero playback glide: %llu calls\n",(unsigned long long)movingGlideHits);
    std::printf("Real playback pitch cache: %llu composed calls\n",(unsigned long long)pitchCacheHits);
    std::printf("Real playback pitch conversion/cache: %llu calls, %llu instructions\n",
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
    collectH8Fallback = false;
    if (profile) {
        uint64_t remaining = 0, pitchRemaining = 0;
        std::vector<std::pair<uint64_t,unsigned>> fallbackBuckets;
        for (unsigned base = 0; base < h8FallbackCounts.size(); base += 256) {
            uint64_t sum = 0;
            for (unsigned j = 0; j < 256; ++j) sum += h8FallbackCounts[base+j];
            remaining += sum; fallbackBuckets.emplace_back(sum,base);
        }
        for (unsigned pc = 0x4f51; pc <= 0x53e4; ++pc) pitchRemaining += h8FallbackCounts[pc];
        std::sort(fallbackBuckets.rbegin(),fallbackBuckets.rend());
        std::printf("Native playback H8 fallback: %llu instructions, pitch region %llu\n",
                    (unsigned long long)remaining,(unsigned long long)pitchRemaining);
        uint64_t filterRemaining = 0;
        std::vector<std::pair<uint64_t, unsigned>> filterFallback;
        for (unsigned pc = 0x4443; pc <= 0x4856; ++pc) {
            filterRemaining += h8FallbackCounts[pc];
            if (h8FallbackCounts[pc]) filterFallback.emplace_back(h8FallbackCounts[pc],pc);
        }
        std::sort(filterFallback.rbegin(),filterFallback.rend());
        std::printf("Filter region fallback: %llu instructions\n",(unsigned long long)filterRemaining);
        for (unsigned mask = 0; mask < 8; ++mask)
            if (filterLfoFallbackMasks[mask])
                std::printf("Filter LFO fallback mask %u: %llu\n",mask,
                            (unsigned long long)filterLfoFallbackMasks[mask]);
        for (size_t i = 0; i < std::min(size_t(20),filterFallback.size()); ++i)
            std::printf("Filter fallback %04x %llu\n",filterFallback[i].second,
                        (unsigned long long)filterFallback[i].first);
        for (unsigned i = 0; i < 20; ++i)
            std::printf("Fallback %06x %llu %.2f%%\n",fallbackBuckets[i].second,
                        (unsigned long long)fallbackBuckets[i].first,
                        remaining ? 100.0*fallbackBuckets[i].first/remaining : 0);
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
