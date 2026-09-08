// Capture the firmware/DSP boundary before replacing the firmware in C++.
// This offline tool deliberately records every write, including repeated values.
#include "emu.h"
#include "audio.h"
#include "rom_loader.h"
#include "sc55_patch.h"
#include "sc55_midi.h"
#include "sc55_channel.h"
#include "sc55_preset.h"
#include "sc55_voice_setup.h"
#include "sc55_sample_bank.h"
#include "sc55_sound_data.h"
#include "sc55_note_setup.h"
#include "sc55_envelope_setup.h"
#include "sc55_envelope.h"
#include "sc55_envelope_pcm.h"
#include "uart-status-test.h"
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <set>
#include <memory>
#include <algorithm>
#include "envelope-runner-oracle.h"
#include "envelope-key-import.h"
#include "envelope-pcm-test.h"
#include "voice-control-pcm-test.h"
#include "native-player-test.h"
#include "rhythm-velocity-oracle.h"
#include "control-clock-probe.h"
#include "native-tva-test.h"

namespace {
FILE* trace = nullptr;
uint64_t frames = 0, writes = 0, nonzero = 0;
const char* phase = "boot";
std::array<std::pair<uint8_t,uint8_t>,11> expectedSetup;
unsigned setupPosition = 11;
uint64_t setupChecks = 0;
std::optional<sc55::EnvelopePcmSync> expectedEnvelopeSync;
unsigned syncByte = 0;
uint64_t syncWriteChecks = 0;
std::array<std::pair<uint8_t,uint8_t>,3> expectedTermination;
unsigned terminationByte = 3;
uint64_t terminationWriteChecks = 0;
void sample(void*, const AudioFrame<int32_t>& frame)
{
    ++frames;
    if (frame.left != 0 || frame.right != 0) ++nonzero;
}

// Isolated instruction tests: execute the actual ROM instructions without
// peripherals/interrupts. This is not a timing or whole-note equivalence test.
void verifyKeyArithmetic(std::span<const uint8_t> rom1,std::span<const uint8_t> rom2)
{
    auto cpu = std::make_unique<mcu_t>();
    if (rom1.size() != 32768) throw std::runtime_error("Unexpected internal ROM size");
    std::copy(rom1.begin(), rom1.end(), cpu->rom1);
    cpu->is_mk1 = true;
    cpu->ep = cpu->cp = cpu->dp = 0;
    const auto envelopeKeys = importEnvelopeKeys(rom1,rom2);
    const auto envelopeKeyLevels = importEnvelopeKeyLevels(rom1,rom2);
    sc55::EnvelopeLevelTables levelTables;
    std::copy_n(rom1.begin()+0x6b0f,128,levelTables.attenuation.begin());
    std::copy_n(rom1.begin()+0x6b8f,256,levelTables.level.begin());
    std::copy(rom2.begin(),rom2.end(),cpu->rom2); cpu->rom2_mask = 0x3ffff;
    unsigned keyScaleChecks = 0;
    for (unsigned selector = 0; selector < 16; ++selector)
        for (unsigned key = 0; key < 256; ++key)
            for (unsigned sensitivity = 44; sensitivity <= 84; ++sensitivity)
            {
                SC55Partial partial{};
                partial.raw[0x55] = uint8_t(selector); partial.raw[0x56] = uint8_t(15-selector);
                partial.raw[0x57] = uint8_t(sensitivity); partial.raw[0x58] = uint8_t(128-sensitivity);
                for (unsigned i = 0x55; i <= 0x58; ++i) MCU_Write(*cpu,0x9200+i,partial.raw[i]);
                MCU_Write16(*cpu,0x93fe,0); MCU_Write(*cpu,0xc8fc,uint8_t(key));
                cpu->cp = cpu->dp = cpu->ep = 0; cpu->sr = 0; cpu->pc = 0x2d67;
                cpu->r[0] = 0x9400; cpu->r[5] = 0x9200; cpu->r[2] = 0;
                unsigned steps = 0;
                while (cpu->pc != 0x2e30)
                {
                    if (++steps > 110) throw std::runtime_error("Envelope key scale escaped");
                    const auto opcode = MCU_ReadCodeAdvance(*cpu); MCU_Operand_Table[opcode](*cpu,opcode);
                }
                const auto predicted = sc55::PrepareEnvelopeKeyScales(partial,uint8_t(key),envelopeKeys);
                const auto word = [&](uint16_t at) { return uint16_t((MCU_Read(*cpu,at)<<8)|MCU_Read(*cpu,at+1)); };
                if (!predicted || (*predicted)[0] != word(0x93de) || (*predicted)[1] != word(0x93e0))
                    throw std::runtime_error("Envelope key scale differs from firmware");
                ++keyScaleChecks;
                partial.raw[0x45] = uint8_t((key+selector)%128);
                partial.raw[0x46] = uint8_t(selector); partial.raw[0x47] = uint8_t(sensitivity);
                for (unsigned i = 0x45; i <= 0x47; ++i) MCU_Write(*cpu,0x9200+i,partial.raw[i]);
                cpu->pc = 0x2c6a; cpu->sr = 0; cpu->cp = cpu->dp = cpu->ep = 0; steps = 0;
                while (cpu->pc != 0x2cda)
                {
                    if (++steps > 70) throw std::runtime_error("Envelope key level escaped");
                    const auto opcode = MCU_ReadCodeAdvance(*cpu); MCU_Operand_Table[opcode](*cpu,opcode);
                }
                if (sc55::PrepareEnvelopeKeyLevel(partial,uint8_t(key),levelTables,envelopeKeyLevels) != cpu->r[2])
                    throw std::runtime_error("Envelope key level differs from firmware");
            }
    std::printf("Native envelope key scales: %u paired combinations matched\n",keyScaleChecks);
    std::printf("Native envelope key levels: %u combinations matched\n",keyScaleChecks);
    unsigned releaseChecks = 0;
    for (unsigned stage = 0; stage < 7; ++stage)
        for (unsigned level = 0; level < 65536; level += 257)
            for (uint16_t delay : {uint16_t(0),uint16_t(1),uint16_t(65535)})
            {
                cpu->pc = 0x3220; cpu->sr = 0; cpu->r[0] = 0x9400; cpu->r[1] = 0;
                MCU_Write16(*cpu,0x9400,uint16_t(stage*2)); MCU_Write16(*cpu,0x9410,delay);
                MCU_Write16(*cpu,0x941c,uint16_t(level)); MCU_Write16(*cpu,0x9408,123); MCU_Write16(*cpu,0x9412,7);
                MCU_Write(*cpu,0x944f,10); MCU_Write(*cpu,0x93f8,0); MCU_Write(*cpu,0x9460,12); MCU_Write(*cpu,0x9461,100);
                MCU_Write(*cpu,0x9453,55); MCU_Write(*cpu,0x93fc,4);
                const auto expected = sc55::ReleaseEnvelope({static_cast<sc55::EnvelopeStage>(stage),{123,7},{10,0},12,100},
                    uint16_t(level),{55,4},delay);
                unsigned steps = 0;
                while (cpu->pc != 0x328b && cpu->pc != 0x32f0 && cpu->pc != 0x3363)
                {
                    if (++steps > 40) throw std::runtime_error("Envelope release probe escaped routine");
                    const auto opcode = MCU_ReadCodeAdvance(*cpu); MCU_Operand_Table[opcode](*cpu,opcode);
                }
                const auto word = [&](uint16_t at) { return uint16_t((MCU_Read(*cpu,at)<<8)|MCU_Read(*cpu,at+1)); };
                const unsigned code = expected.stage == sc55::EnvelopeStage::finished ? 22 : unsigned(expected.stage)*2;
                if (word(0x9400) != code || word(0x9408) != expected.progress.position || word(0x9412) != expected.progress.deferredTicks
                    || MCU_Read(*cpu,0x944f) != expected.parameter.value || MCU_Read(*cpu,0x93f8) != expected.parameter.flag
                    || MCU_Read(*cpu,0x9460) != expected.start || MCU_Read(*cpu,0x9461) != expected.target)
                    throw std::runtime_error("Envelope release differs from firmware");
                ++releaseChecks;
            }
    std::printf("Native envelope release: %u combinations matched\n",releaseChecks);
    unsigned transitionChecks = 0;
    for (unsigned stage = 0; stage < 7; ++stage)
        for (uint16_t progress : {uint16_t(0),uint16_t(1),uint16_t(65534),uint16_t(65535)})
            for (unsigned value = 0; value < 256; ++value)
            {
                cpu->pc = 0x33f4; cpu->sr = 0; cpu->r[0] = 0x9400;
                MCU_Write16(*cpu,0x9400,uint16_t(stage*2)); MCU_Write16(*cpu,0x9408,progress); MCU_Write16(*cpu,0x9412,3);
                std::array<SC55ExpandedParameter,5> parameters;
                std::array<uint8_t,4> targets;
                for (unsigned i = 0; i < 5; ++i)
                {
                    parameters[i] = SC55_ExpandParameter(uint8_t(value+i));
                    MCU_Write(*cpu,0x944f+i,parameters[i].value); MCU_Write(*cpu,0x93f8+i,parameters[i].flag);
                }
                for (unsigned i = 0; i < 4; ++i) { targets[i] = uint8_t(value+10+i); MCU_Write(*cpu,0x9461+i,targets[i]); }
                MCU_Write(*cpu,0x9460,42);
                const auto expected = sc55::AdvanceEnvelopeStage(
                    {static_cast<sc55::EnvelopeStage>(stage),{progress,3},parameters[0],42,targets[0]},parameters,targets);
                unsigned steps = 0;
                while (cpu->pc != 0x3445 && cpu->pc != 0x3472)
                {
                    if (++steps > 40) throw std::runtime_error("Envelope stage probe escaped routine");
                    const uint8_t opcode = MCU_ReadCodeAdvance(*cpu); MCU_Operand_Table[opcode](*cpu,opcode);
                }
                const auto word = [&](uint16_t at) { return uint16_t((MCU_Read(*cpu,at)<<8)|MCU_Read(*cpu,at+1)); };
                const unsigned expectedCode = expected.stage == sc55::EnvelopeStage::finished ? 22 : unsigned(expected.stage)*2;
                if (word(0x9400) != expectedCode || word(0x9408) != expected.progress.position || word(0x9412) != expected.progress.deferredTicks
                    || MCU_Read(*cpu,0x944f) != expected.parameter.value || MCU_Read(*cpu,0x93f8) != expected.parameter.flag
                    || MCU_Read(*cpu,0x9460) != expected.start || MCU_Read(*cpu,0x9461) != expected.target)
                    throw std::runtime_error("Envelope stage transition differs from firmware");
                ++transitionChecks;
            }
    std::printf("Native envelope transitions: %u combinations matched\n",transitionChecks);
    sc55::EnvelopeTimes times;
    for (unsigned i = 0; i < times.size(); ++i) times[i] = uint16_t((rom1[0x6f12+2*i]<<8)|rom1[0x6f13+2*i]);
    verifyEnvelopeRunner(*cpu,times);
    unsigned durationChecks = 0;
    for (unsigned stage = 0; stage < 128; ++stage)
        for (unsigned control = 0; control < 128; ++control)
            for (const auto scales : std::array<std::array<uint16_t,2>,5>{{{0,0},{256,256},{65535,65535},{255,257},{1,65535}}})
            {
                cpu->pc = 0x348c; cpu->sr = 0; cpu->r[0] = 0x9400;
                MCU_Write16(*cpu,0x942e,0x9000); MCU_Write(*cpu,0x9014,uint8_t(control));
                MCU_Write(*cpu,0x944f,uint8_t(stage)); MCU_Write16(*cpu,0x93de,scales[0]); MCU_Write16(*cpu,0x93e2,scales[1]);
                unsigned steps = 0;
                while (cpu->pc != 0x358e)
                {
                    if (++steps > 60) throw std::runtime_error("Envelope duration probe escaped routine");
                    const uint8_t opcode = MCU_ReadCodeAdvance(*cpu);
                    MCU_Operand_Table[opcode](*cpu,opcode);
                }
                if (sc55::PrepareEnvelopeDuration(uint8_t(stage),uint8_t(control),scales[0],scales[1],times) != cpu->r[6])
                    throw std::runtime_error("Envelope duration differs from firmware");
                ++durationChecks;
            }
    std::printf("Native envelope duration: %u combinations matched\n",durationChecks);
    unsigned shortEnvelopeChecks = 0;
    for (unsigned duration = 0; duration <= 8; ++duration)
        for (unsigned target = 0; target < 256; ++target)
            for (uint16_t previous : {uint16_t(0),uint16_t(1),uint16_t(255),uint16_t(256),uint16_t(32768),uint16_t(65280),uint16_t(65535)})
            {
                cpu->pc = 0x358e; cpu->sr = 0; cpu->r[0] = 0x9400;
                cpu->r[3] = cpu->r[6] = uint16_t(duration);
                MCU_Write(*cpu,0x9461,uint8_t(target)); MCU_Write16(*cpu,0x941c,previous);
                MCU_Write16(*cpu,0x9408,123); MCU_Write16(*cpu,0x9412,7);
                unsigned steps = 0;
                while (cpu->pc != 0x36a7 && cpu->pc != 0x36ad && cpu->pc != 0x36da)
                {
                    if (++steps > 80) throw std::runtime_error("Short envelope probe escaped routine");
                    const uint8_t opcode = MCU_ReadCodeAdvance(*cpu);
                    MCU_Operand_Table[opcode](*cpu,opcode);
                }
                const auto predicted = sc55::StepEnvelopeSegment(uint16_t(duration),999,{123,7},42,uint8_t(target),false,previous);
                const auto word = [&](uint16_t at) { return uint16_t((MCU_Read(*cpu,at)<<8)|MCU_Read(*cpu,at+1)); };
                if (!predicted || word(0x941c) != predicted->level || word(0x941e) != predicted->pcmWord
                    || word(0x9408) != predicted->progress.position || word(0x9412) != predicted->progress.deferredTicks)
                    throw std::runtime_error("Short envelope dispatch differs from firmware");
                ++shortEnvelopeChecks;
            }
    std::printf("Native short envelope updates: %u combinations matched\n",shortEnvelopeChecks);
    unsigned progressChecks = 0;
    for (unsigned duration = 9; duration < 65536; ++duration)
        for (const auto input : std::array<std::array<uint16_t,3>,5>{{
            {0,0,0},{1,0,0},{1,65530,1},{65535,65535,0},{65535,7,2}}})
        {
            cpu->pc = 0x3599; cpu->sr = 0; cpu->r[0] = 0x9400; cpu->r[6] = uint16_t(duration);
            MCU_Write16(*cpu,0xac5a,input[0]); MCU_Write16(*cpu,0x9408,input[1]); MCU_Write16(*cpu,0x9412,input[2]);
            unsigned steps = 0;
            while (cpu->pc != 0x35c9)
            {
                if (++steps > 32) throw std::runtime_error("Envelope progress probe escaped routine");
                const uint8_t opcode = MCU_ReadCodeAdvance(*cpu);
                MCU_Operand_Table[opcode](*cpu,opcode);
            }
            const auto expected = sc55::AdvanceEnvelopeProgress(uint16_t(duration),input[0],{input[1],input[2]});
            if (!expected || expected->position != cpu->r[3]
                || expected->deferredTicks != uint16_t((MCU_Read(*cpu,0x9412)<<8)|MCU_Read(*cpu,0x9413)))
                throw std::runtime_error("Envelope progress differs from firmware");
            ++progressChecks;
        }
    std::printf("Native envelope progress: %u combinations matched\n",progressChecks);
    unsigned envelopeChecks = 0;
    for (unsigned amplitude = 0; amplitude < 256; ++amplitude)
        for (unsigned sensitivity = 44; sensitivity <= 84; ++sensitivity)
            for (unsigned operation = 0; operation < 2; ++operation)
            {
                cpu->pc = operation == 0 ? 0x2e30 : 0x2e85;
                cpu->sr = 0; cpu->r[0] = 0x9400; cpu->r[1] = 0; cpu->r[5] = 0x9000;
                MCU_Write(*cpu,0xc944,uint8_t(amplitude));
                MCU_Write(*cpu,0x9059+operation,uint8_t(sensitivity));
                const uint16_t stop = operation == 0 ? 0x2e85 : 0x2eda;
                unsigned steps = 0;
                while (cpu->pc != stop)
                {
                    if (++steps > 40) throw std::runtime_error("Envelope scale probe escaped routine");
                    const uint8_t opcode = MCU_ReadCodeAdvance(*cpu);
                    MCU_Operand_Table[opcode](*cpu,opcode);
                }
                const uint16_t at = uint16_t(0x93e2+operation*2);
                const auto actual = uint16_t((MCU_Read(*cpu,at)<<8) | MCU_Read(*cpu,at+1));
                if (sc55::EnvelopeVelocityScale(uint8_t(amplitude),uint8_t(sensitivity)) != actual)
                    throw std::runtime_error("Envelope velocity scale differs from firmware");
                ++envelopeChecks;
            }
    std::printf("Native envelope velocity scales: %u combinations matched\n",envelopeChecks);
    unsigned sampleControlChecks = 0;
    for (unsigned history = 0; history < 16; ++history)
    for (unsigned high = 0; high < 256; ++high)
        for (unsigned flags : {0u,1u,2u,3u,252u,253u,254u,255u})
            for (unsigned voice = 0; voice < 24; ++voice)
            {
                cpu->pc = 0x2bef; cpu->sr = 0;
                cpu->r[0] = 0x9400; cpu->r[1] = uint16_t(voice);
                cpu->r[3] = uint16_t((history << 8) | high); cpu->r[5] = 0x9000;
                MCU_Write(*cpu,0x900a,uint8_t(flags));
                unsigned steps = 0;
                while (cpu->pc != 0x2c13)
                {
                    if (++steps > 24) throw std::runtime_error("Sample control probe escaped routine");
                    const uint8_t opcode = MCU_ReadCodeAdvance(*cpu);
                    MCU_Operand_Table[opcode](*cpu,opcode);
                }
                const auto expected = sc55::DecodeSampleControl(high << 16,uint8_t(flags),uint8_t(voice),uint8_t(history));
                const auto actual = uint16_t((MCU_Read(*cpu,0x93f6) << 8) | MCU_Read(*cpu,0x93f7));
                if (actual != expected.mode || MCU_Read(*cpu,0x93ef) != expected.loopFlag)
                    throw std::runtime_error("Sample control arithmetic differs from firmware");
                ++sampleControlChecks;
            }
    std::printf("Native sample control: %u bank/flags/voice combinations matched\n",sampleControlChecks);
    cpu->r[5] = 0x9000; // synthetic partial in SRAM, no ROM modification
    unsigned checks = 0;
    for (unsigned operation = 0; operation < 2; ++operation)
        for (unsigned key = 0; key < 256; ++key)
            for (unsigned parameter = 0; parameter < 256; ++parameter)
            {
                cpu->pc = operation == 0 ? 0x1378 : 0x1361;
                const uint16_t stop = operation == 0 ? 0x138f : 0x1374;
                cpu->r[0] = uint16_t(key);
                cpu->sr = 0;
                MCU_Write(*cpu, 0x9000 + (operation == 0 ? 10 : 1), uint8_t(parameter));
                unsigned steps = 0;
                while (cpu->pc != stop)
                {
                    if (++steps > 20) throw std::runtime_error("Key arithmetic probe escaped routine");
                    const uint8_t opcode = MCU_ReadCodeAdvance(*cpu);
                    MCU_Operand_Table[opcode](*cpu, opcode);
                }
                const auto expected = operation == 0
                    ? sc55::TransposePartialKey(uint8_t(key), uint8_t(parameter))
                    : sc55::MakeSampleLookupKey(uint8_t(key), uint8_t(parameter));
                if (uint8_t(cpu->r[0]) != expected)
                    throw std::runtime_error("Native key arithmetic differs from firmware");
                ++checks;
            }
    std::printf("Native key arithmetic: %u byte-input pairs matched\n", checks);
    unsigned trackingChecks = 0;
    for (unsigned key = 0; key < 128; ++key)
        for (unsigned source = 0; source < 128; ++source)
            for (unsigned tracking = 44; tracking <= 84; ++tracking)
            {
                cpu->pc = 0x1390;
                cpu->r[0] = uint16_t(key);
                cpu->r[5] = 0x9000;
                cpu->r[7] = 0x9200;
                cpu->sr = 0;
                MCU_Write(*cpu, 0x900d, uint8_t(tracking));
                MCU_Write(*cpu, 0xa1b4, uint8_t(source));
                unsigned steps = 0;
                while (cpu->pc != 0x141e)
                {
                    if (++steps > 80) throw std::runtime_error("Key tracking escaped routine");
                    const uint8_t opcode = MCU_ReadCodeAdvance(*cpu);
                    MCU_Operand_Table[opcode](*cpu, opcode);
                }
                const auto expected = sc55::TrackPartialKey(uint8_t(key), uint8_t(source), uint8_t(tracking));
                const uint16_t fraction = uint16_t((MCU_Read(*cpu, 0xa1ca) << 8) | MCU_Read(*cpu, 0xa1cb));
                if (!expected || expected->key != uint8_t(cpu->r[0]) || expected->fraction != fraction)
                {
                    std::fprintf(stderr, "tracking key=%u source=%u tracking=%u firmware=%u/%u native=%u/%u\n",
                                 key, source, tracking, uint8_t(cpu->r[0]), fraction,
                                 expected ? expected->key : 255, expected ? expected->fraction : 65535);
                    throw std::runtime_error("Native key tracking differs from firmware");
                }
                ++trackingChecks;
            }
    std::printf("Native key tracking: %u key/source/slope combinations matched\n", trackingChecks);
    unsigned tuningChecks = 0;
    for (unsigned key : {0u, 60u, 127u, 255u})
        for (unsigned fraction : {0u, 1u, 9u, 10u, 499u, 500u, 990u, 999u, 1000u})
            for (unsigned tuning = 0; tuning < 128; ++tuning)
                for (unsigned slopeIndex = 0; slopeIndex < 42; ++slopeIndex)
                {
                    const unsigned tracking = slopeIndex == 0 ? 0 : slopeIndex + 43;
                    cpu->pc = 0x141f;
                    cpu->r[0] = uint16_t(key);
                    cpu->r[5] = 0x9000;
                    cpu->r[7] = 0x9200;
                    cpu->sr = 0;
                    MCU_Write(*cpu, 0x900d, uint8_t(tracking));
                    MCU_Write(*cpu, 0xa1b2, uint8_t(key));
                    MCU_Write16(*cpu, 0xa1c0, 0x9400);
                    // Distinct values in other pitch classes expose indexing errors.
                    for (unsigned pitchClass = 0; pitchClass < 12; ++pitchClass)
                        MCU_Write(*cpu, 0x941a + pitchClass,
                                  uint8_t(pitchClass == key % 12 ? tuning : 64));
                    MCU_Write16(*cpu, 0xa1ca, uint16_t(fraction));
                    unsigned steps = 0;
                    while (cpu->pc != 0x14c4)
                    {
                        if (++steps > 100) throw std::runtime_error("Scale tuning escaped routine");
                        const uint8_t opcode = MCU_ReadCodeAdvance(*cpu);
                        MCU_Operand_Table[opcode](*cpu, opcode);
                    }
                    const auto expected = sc55::ApplyScaleTuning({uint8_t(key), uint16_t(fraction)},
                                                                uint8_t(tuning), uint8_t(tracking));
                    const uint16_t actualFraction = uint16_t((MCU_Read(*cpu, 0xa1ca) << 8) | MCU_Read(*cpu, 0xa1cb));
                    if (!expected || expected->key != uint8_t(cpu->r[0]) || expected->fraction != actualFraction)
                        throw std::runtime_error("Native scale tuning differs from firmware");
                    ++tuningChecks;
                }
    std::printf("Native scale tuning: %u key/fraction/tuning/slope combinations matched\n", tuningChecks);
    unsigned resolutionChecks = 0;
    for (unsigned mode = 0; mode < 256; ++mode)
        for (unsigned key = 0; key < 256; ++key)
            for (unsigned minimum : {0u, 1u, 63u, 127u, 128u, 254u, 255u})
                for (unsigned reference : {0u, 64u, 127u, 255u})
                {
                    const uint8_t remapped = uint8_t(255-minimum);
                    cpu->pc = 0x1334;
                    cpu->r[0] = uint16_t(key);
                    cpu->r[1] = uint16_t(minimum);
                    cpu->r[5] = 0x9000;
                    cpu->sr = 0;
                    cpu->r[7] = 0x9200;
                    MCU_Write(*cpu,0xa1cc,uint8_t(minimum));
                    MCU_Write(*cpu, 0xa3d1, uint8_t(mode));
                    MCU_Write16(*cpu, 0xa1c6, 0x9400);
                    MCU_Write(*cpu, 0x9580, remapped);
                    MCU_Write(*cpu, 0x9001, uint8_t(reference));
                    MCU_Write(*cpu, 0xa1b2, 0x55);
                    MCU_Write(*cpu, 0xa1b3, 0xaa);
                    unsigned steps = 0;
                    while (cpu->pc != 0x1374)
                    {
                        // Include the caller's minimum-key reload and BSR.
                        if (cpu->pc == 0x1356) cpu->pc = 0x123d;
                        if (++steps > 40) throw std::runtime_error("Partial key resolution escaped routine");
                        const uint8_t opcode = MCU_ReadCodeAdvance(*cpu);
                        MCU_Operand_Table[opcode](*cpu, opcode);
                    }
                    const auto expected = sc55::ResolvePartialKey(uint8_t(key),uint8_t(minimum),
                        uint8_t(mode),remapped,uint8_t(reference));
                    if (expected.lookupKey != uint8_t(cpu->r[0])
                        || expected.storedOriginalNote.value_or(0x55) != MCU_Read(*cpu,0xa1b2)
                        || expected.storedAdjustedKey.value_or(0xaa) != MCU_Read(*cpu,0xa1b3))
                        throw std::runtime_error("Native partial key resolution differs from firmware");
                    ++resolutionChecks;
                }
    std::printf("Native partial key resolution: %u mode/key/minimum/reference combinations matched\n", resolutionChecks);
    unsigned transposeChecks = 0;
    for (unsigned operation = 0; operation < 2; ++operation)
        for (unsigned key = 0; key < 256; ++key)
            for (unsigned shift = 0; shift < 256; ++shift)
                for (unsigned offset : {0u,1u,63u,127u,128u,254u,255u})
                {
                    if (operation == 1 && offset != 0) continue;
                    cpu->pc = operation == 0 ? 0x1e1b : 0x1e3a;
                    const uint16_t stop = operation == 0 ? 0x1e39 : 0x1e58;
                    cpu->r[0] = cpu->r[1] = uint16_t(key);
                    cpu->r[2] = 0x9400;
                    cpu->r[3] = 0;
                    cpu->sr = 0;
                    MCU_Write(*cpu,0x9406,uint8_t(shift));
                    MCU_Write(*cpu,0xab46,uint8_t(offset));
                    MCU_Write(*cpu,0x8005,uint8_t(shift));
                    unsigned steps = 0;
                    while (cpu->pc != stop)
                    {
                        if (++steps > 20) throw std::runtime_error("Part/master transpose escaped routine");
                        const uint8_t opcode = MCU_ReadCodeAdvance(*cpu);
                        MCU_Operand_Table[opcode](*cpu, opcode);
                    }
                    const auto expected = operation == 0
                        ? sc55::TransposePartKey(uint8_t(key),uint8_t(shift),uint8_t(offset))
                        : sc55::TransposeMasterKey(uint8_t(key),uint8_t(shift));
                    if (uint8_t(cpu->r[0]) != expected) throw std::runtime_error("Native part/master transpose differs from firmware");
                    ++transposeChecks;
                }
    std::printf("Native part/master transpose: %u input combinations matched\n", transposeChecks);
    unsigned velocityChecks = 0;
    for (unsigned velocity = 0; velocity < 128; ++velocity)
        for (unsigned first = 0; first < 128; ++first)
            for (unsigned last = 0; last < 128; ++last)
            {
                cpu->pc = 0x1034;
                cpu->r[2] = uint16_t(velocity);
                cpu->r[3] = 0;
                cpu->r[5] = 0x9000;
                cpu->r[7] = 0x9200;
                cpu->sr = 0;
                MCU_Write(*cpu,0x9041,uint8_t(first));
                MCU_Write(*cpu,0x9043,uint8_t(last));
                unsigned steps = 0;
                while (cpu->pc != 0x1082 && cpu->pc != 0x1119)
                {
                    if (++steps > 45) throw std::runtime_error("Velocity interval probe escaped routine");
                    const uint8_t opcode = MCU_ReadCodeAdvance(*cpu);
                    MCU_Operand_Table[opcode](*cpu,opcode);
                }
                const auto index = sc55::PartialVelocityCurveIndex(uint8_t(velocity),uint8_t(first),uint8_t(last));
                if (bool(index) != (cpu->pc == 0x1082) || (index && *index != uint8_t(cpu->r[2])))
                    throw std::runtime_error("Native velocity interval/index differs from firmware");
                ++velocityChecks;
            }
    std::printf("Native velocity interval/index: %u combinations matched\n",velocityChecks);
    unsigned amplitudeChecks = 0;
    for (unsigned accumulator : {0u,1u,63u,127u,128u,254u,255u})
        for (unsigned increment : {0u,1u,63u,127u,128u,254u,255u})
            for (unsigned endpoint : {0u,1u,63u,127u,128u,254u,255u})
                for (unsigned curve = 0; curve < 256; ++curve)
                {
                    cpu->pc = 0x109b; cpu->sr = 0;
                    cpu->r[0] = 0; cpu->r[2] = uint16_t(curve);
                    cpu->r[5] = 0x9000; cpu->r[7] = 0x9200;
                    MCU_Write(*cpu,0xa1b7,uint8_t(accumulator));
                    MCU_Write(*cpu,0x9042,uint8_t(increment));
                    MCU_Write(*cpu,0x9044,uint8_t(endpoint));
                    unsigned steps = 0;
                    while (cpu->pc != 0x10df)
                    {
                        if (++steps > 40) throw std::runtime_error("Velocity interpolation escaped routine");
                        const uint8_t opcode = MCU_ReadCodeAdvance(*cpu);
                        MCU_Operand_Table[opcode](*cpu,opcode);
                    }
                    const auto expected = sc55::InterpolateVelocityAmplitude(uint8_t(accumulator),uint8_t(increment),
                        uint8_t(endpoint),uint8_t(curve));
                    if (expected.first != MCU_Read(*cpu,0xa1b7) || expected.second != MCU_Read(*cpu,0xa1b8))
                        throw std::runtime_error("Native velocity interpolation differs");
                    ++amplitudeChecks;
                }
    std::printf("Native velocity interpolation: %u combinations matched\n",amplitudeChecks);
}
}

void Oracle_PCM_Write(pcm_t& pcm, uint32_t address, uint8_t value)
{
    if (terminationByte < expectedTermination.size())
    {
        const auto expected = expectedTermination[terminationByte++];
        if (address != expected.first || value != expected.second) throw std::runtime_error("Native PCM termination differs");
        ++terminationWriteChecks;
    }
    if (expectedEnvelopeSync)
    {
        if (syncByte >= 2 || address != expectedEnvelopeSync->address+syncByte
            || value != uint8_t(expectedEnvelopeSync->word >> (syncByte == 0 ? 8 : 0)))
            throw std::runtime_error("Native PCM envelope sync write differs");
        ++syncWriteChecks;
        if (++syncByte == 2) expectedEnvelopeSync.reset();
    }
    if (setupPosition < expectedSetup.size())
    {
        const auto expected = expectedSetup[setupPosition++];
        if (expected.first != address || expected.second != value)
            throw std::runtime_error("Native sample-address transaction mismatch");
        ++setupChecks;
    }
    if (trace != nullptr)
    {
        const auto& mcu = *pcm.mcu;
        // PC is the CPU's PC at the device access, not the instruction start.
        std::fprintf(trace, "%s,%llu,%llu,%02x:%04x,%02x,%02x\n", phase,
                     static_cast<unsigned long long>(frames),
                     static_cast<unsigned long long>(mcu.cycles),
                     mcu.cp, mcu.pc, address, value);
        ++writes;
    }
    PCM_Write(pcm, address, value);
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--native-tva-test")
    {
        try { return verifyNativeTva (argv[2]); }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "Native TVA test: %s\n", error.what());
            return 1;
        }
    }
    if (argc == 4 && std::string(argv[1]) == "--native-player-test")
        return verifyNativePlayer(argv[2],argv[3]);
    if (argc == 4 && std::string(argv[1]) == "--native-wave-output-test")
        return verifyNativeVoiceControlPcm(argv[2],argv[3]);
    if (argc == 3 && std::string(argv[1]) == "--native-voice-control-pcm-test")
        return verifyNativeVoiceControlPcm(argv[2]);
    if (argc == 2 && std::string(argv[1]) == "--native-envelope-pcm-test")
        return verifyNativeEnvelopePcm();
    if (argc == 2 && std::string(argv[1]) == "--uart-status-test")
        return verifyUartStatusRace() ? 0 : 1;
    const bool patchMode = argc == 4 && std::string(argv[3]) == "--patch-probes";
    const bool monoMode = argc == 4 && std::string(argv[3]) == "--mono-probes";
    const bool holdMode = monoMode || (argc == 4 && std::string(argv[3]) == "--hold-probes");
    const bool outputControlMode = argc == 4 && std::string(argv[3]) == "--output-control-probes";
    const bool midiMode = argc == 4 && std::string(argv[3]) == "--native-midi";
    const bool rpnMode = argc == 4 && std::string(argv[3]) == "--rpn-probes";
    const bool controllerMode = rpnMode || (argc == 4 && std::string(argv[3]) == "--controller-probes");
    const bool softMode = argc == 4 && std::string(argv[3]) == "--soft-sample-probes";
    const bool tunedMode = softMode || (argc == 4 && std::string(argv[3]) == "--tuned-sample-probes");
    const bool allSoundExportMode = argc == 4 && std::string(argv[3]) == "--export-all-sound-data";
    const bool soundExportMode = allSoundExportMode || (argc == 4 && std::string(argv[3]) == "--export-sound-data");
    const bool allSampleExportMode = allSoundExportMode || (argc == 4 && std::string(argv[3]) == "--export-all-sample-bank");
    const bool exportMode = soundExportMode || allSampleExportMode || (argc == 4 && std::string(argv[3]) == "--export-sample-bank");
    const bool sampleMode = exportMode || tunedMode || (argc == 4 && std::string(argv[3]) == "--sample-probes");
    const bool programMode = sampleMode || (argc == 4 && std::string(argv[3]) == "--program-probes");
    if (argc != 3 && !patchMode && !midiMode && !controllerMode && !programMode && !holdMode && !outputControlMode)
    {
        std::fprintf(stderr, "Usage: sc55-firmware-oracle ROM_DIRECTORY NEW_TRACE.csv [--patch-probes|--native-midi|--controller-probes|--program-probes|--hold-probes|--mono-probes|--output-control-probes]\n");
        return 2;
    }
    try
    {
        common::LoadRomsetResult roms;
        const auto error = common::LoadRomset(argv[1], {}, common::RomLoader::Hashing, {}, roms);
        if (error != common::LoadRomsetError{}) throw std::runtime_error(common::ToCString(error));
        if (roms.romset != Romset::MK1) throw std::runtime_error("This scenario currently targets SC-55 mk1 ROMs");
        if ((rpnMode || holdMode || outputControlMode) && roms.picked_name != "mk1-v1.21")
            throw std::runtime_error("RPN/hold probes require the hashed mk1-v1.21 ROM set");
        if (sampleMode)
        {
            if (roms.picked_name != "mk1-v1.21")
                throw std::runtime_error("Sample probes require the hashed mk1-v1.21 ROM set");
            if (!exportMode) verifyKeyArithmetic(roms.romset_info.rom_data[static_cast<size_t>(RomLocation::ROM1)],
                roms.romset_info.rom_data[static_cast<size_t>(RomLocation::ROM2)]);
        }
        if (std::filesystem::exists(argv[2])) throw std::runtime_error("Refusing to overwrite an existing trace");
        Emulator emu;
        sc55::SampleBank sampleBank;
        sc55::SoundData soundData;
        sc55::VelocityCurves velocityCurves{};
        sc55::EnvelopeTimes envelopeTimes{};
        sc55::EnvelopeLevelTables envelopeLevels{};
        SC55PatchTable patches;
        if ((patchMode || programMode || monoMode) && !patches.load(roms.romset_info.rom_data[static_cast<size_t>(RomLocation::ROM2)]))
            throw std::runtime_error("Patch table not found");
        if ((patchMode || programMode || monoMode) && roms.picked_name == "mk1-v1.21")
        {
            if (patches.base() != 0x10000 || patches.size() != 224)
                throw std::runtime_error("v1.21 patch import crossed the multisample boundary");
            std::printf("v1.21 patch import: 224 records, ends before multisample group zero\n");
        }
        if (!emu.Init({}) || !emu.LoadRoms(roms.romset, roms.romset_info))
            throw std::runtime_error("Emulator initialisation failed");
        if (sampleMode || monoMode)
        {
            const auto& rom2 = roms.romset_info.rom_data[static_cast<size_t>(RomLocation::ROM2)];
            const auto& rom1 = roms.romset_info.rom_data[static_cast<size_t>(RomLocation::ROM1)];
            for (unsigned curve = 0; curve < velocityCurves.size(); ++curve)
            {
                const unsigned pointer = (rom1.at(0x111a+curve*2)<<8)|rom1.at(0x111b+curve*2);
                for (unsigned index = 0; index < 256; ++index)
                    velocityCurves[curve][index] = rom2.at(0x30000|uint16_t(pointer+index));
            }
        }
        if (sampleMode)
        {
            const auto& rom2 = roms.romset_info.rom_data[static_cast<size_t>(RomLocation::ROM2)];
            const auto& rom1 = roms.romset_info.rom_data[static_cast<size_t>(RomLocation::ROM1)];
            for (unsigned i = 0; i < envelopeTimes.size(); ++i)
                envelopeTimes[i] = uint16_t((rom1.at(0x6f12+2*i)<<8)|rom1.at(0x6f13+2*i));
            std::copy_n(rom1.begin()+0x6b0f,128,envelopeLevels.attenuation.begin());
            std::copy_n(rom1.begin()+0x6b8f,256,envelopeLevels.level.begin());
            std::set<uint16_t> groupIds, sampleIds;
            for (int i = 0; i < patches.size(); ++i)
                for (const auto& partial : patches[i].partial)
                    if (partial.used) groupIds.insert(uint16_t((partial.raw[2] << 8) | partial.raw[3]));
            if (allSampleExportMode)
            {
                auto supplemental = std::make_unique<SC55PatchTable>();
                if (!supplemental->loadRecords(rom2,0x20000,162))
                    throw std::runtime_error("Supplemental patch import failed");
                for (int i = 0; i < supplemental->size(); ++i)
                    for (const auto& partial : (*supplemental)[i].partial)
                        if (partial.used) groupIds.insert(uint16_t((partial.raw[2]<<8)|partial.raw[3]));
            }
            std::vector<sc55::SampleBank::Group> groups;
            std::vector<sc55::SampleBank::Sample> samples;
            const auto readRecord = [&](uint32_t offset,auto& data) {
                for (unsigned i = 0; i < data.size(); ++i)
                    data[i] = rom2.at((offset & 0xffff0000u) | uint16_t(offset+i));
            };
            for (auto id : groupIds)
            {
                sc55::SampleBank::Group group{id,{}};
                readRecord(sc55::V121MultisampleOffset(id),group.data);
                for (unsigned key = 0; key < 128; ++key)
                {
                    const auto sample = sc55::SelectSampleZone(group.data,uint8_t(key));
                    if (!sample) throw std::runtime_error("Imported group has an uncovered key");
                    if (!(*sample & 0x8000)) sampleIds.insert(*sample);
                }
                groups.push_back(group);
            }
            for (auto id : sampleIds)
            {
                sc55::SampleBank::Sample sample{id,{}};
                const auto offset = sc55::V121SampleDescriptorOffset(id);
                if (!offset) throw std::runtime_error("Negative sample ID entered descriptor import");
                readRecord(*offset,sample.data);
                samples.push_back(sample);
            }
            if (!sampleBank.load(std::move(groups),std::move(samples))) throw std::runtime_error("Invalid sample bank");
            std::printf("Owned sample bank: %zu groups, %zu descriptors\n",sampleBank.groupCount(),sampleBank.sampleCount());
            const auto encoded = sampleBank.encode();
            sc55::SampleBank restored;
            if (!restored.loadEncoded(encoded) || restored.encode() != encoded)
                throw std::runtime_error("Sample bank round trip failed");
            sampleBank = std::move(restored); // all following queries use deserialized data
            if (allSampleExportMode)
            {
                // Validate the enlarged closure with real H8 group lookup and
                // descriptor addressing, not just native encode/decode symmetry.
                auto& cpu = emu.GetMCU();
                unsigned checks = 0;
                for (auto group : groupIds)
                    for (unsigned key = 0; key < 128; ++key)
                    {
                        cpu.cp = cpu.dp = cpu.ep = 0; cpu.pc = 0x14f0; cpu.sr = 0;
                        cpu.r[0] = key; cpu.r[5] = 0x9000; cpu.r[7] = 0x9900;
                        MCU_Write16(cpu,0x9002,group);
                        unsigned steps = 0;
                        const auto runTo = [&](uint16_t end) {
                            while (cpu.pc != end)
                            {
                                if (++steps > 100) throw std::runtime_error("All-bank sample lookup escaped");
                                const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                            }
                        };
                        runTo(0x1523);
                        const auto selected = sampleBank.select(group,uint8_t(key));
                        if (!selected || cpu.r[4] != *selected)
                            throw std::runtime_error("All-bank sample selection differs from H8");
                        if (!(*selected&0x8000))
                        {
                            runTo(0x1557);
                            const auto expected = sc55::V121SampleDescriptorOffset(*selected);
                            const auto* sample = sampleBank.sample(*selected);
                            if (!sample || !expected || ((uint32_t(cpu.ep)<<16)|cpu.r[5]) != *expected)
                                throw std::runtime_error("All-bank descriptor address differs from H8");
                            for (unsigned i = 0; i < sample->data.size(); ++i)
                                if (sample->data[i] != MCU_Read(cpu,(uint32_t(cpu.ep)<<16)|uint16_t(cpu.r[5]+i)))
                                    throw std::runtime_error("All-bank descriptor data differs from H8");
                        }
                        ++checks;
                    }
                std::printf("All-bank sample closure: %u H8 group/key selections and descriptor checks matched\n",checks);
            }
            const auto envelopeKeys = importEnvelopeKeys(rom1,rom2);
            const auto envelopeKeyLevels = importEnvelopeKeyLevels(rom1,rom2);
            sc55::PitchGlideRates glideRates;
            for (unsigned i = 0; i < glideRates.size(); ++i)
                glideRates[i] = uint16_t((rom1.at(0x7a32+2*i)<<8)|rom1.at(0x7a33+2*i));
            sc55::PartPitchKeyTables pitchKeys{};
            for (unsigned selector = 1; selector < pitchKeys.size(); ++selector)
            {
                const unsigned pointerAt = 0x3dd32+2*selector;
                const unsigned pointer = (rom2.at(pointerAt)<<8)|rom2.at(pointerAt+1);
                for (unsigned key = 0; key < 256; ++key)
                {
                    const unsigned at = 0x30000+((pointer+2*key)&0xffff);
                    pitchKeys[selector][key] = uint16_t((rom2.at(at)<<8)|rom2.at(0x30000+((at+1)&0xffff)));
                }
            }
            sc55::PitchEnvelopeTables pitchEnvelope;
            const auto importWords = [&](auto& values,unsigned at) {
                for (unsigned i = 0; i < values.size(); ++i) values[i] = uint16_t((rom1.at(at+2*i)<<8)|rom1.at(at+2*i+1));
            };
            importWords(pitchEnvelope.depth.base,0x78c6); importWords(pitchEnvelope.depth.velocity,0x78dc); importWords(pitchEnvelope.depth.depth,0x78f2);
            std::copy_n(rom1.begin()+0x79f2,256,pitchEnvelope.curve.begin());
            sc55::SecondEnvelopePcmTables secondEnvelope;
            importWords(secondEnvelope.levelCurve,0x7612);
            std::copy_n(rom1.begin()+0x7714,256,secondEnvelope.smoothingCeiling.begin());
            std::copy_n(rom1.begin()+0x7816,128,secondEnvelope.outputCeiling.begin());
            sc55::SecondEnvelopePreparationTables secondPreparation;
            importWords(secondPreparation.targets.startSensitivity,0x74d2);
            importWords(secondPreparation.targets.velocitySensitivity,0x74fc);
            importWords(secondPreparation.targets.scale,0x7512);
            const auto rom2word = [&](unsigned at) { return uint16_t((rom2.at(at)<<8)|rom2.at(at+1)); };
            for (unsigned selector = 0; selector < 16; ++selector)
            {
                const unsigned keyPointer = rom2word(0x3dcd2+selector*2);
                const unsigned attackPointer = rom2word(0x3dcf2+selector*2), releasePointer = rom2word(0x3dd12+selector*2);
                for (unsigned key = 0; key < 256; ++key)
                {
                    secondPreparation.keys[selector][key] = uint16_t((rom2.at(0x30000+((keyPointer+key*2)&65535))<<8)
                        |rom2.at(0x30000+((keyPointer+key*2+1)&65535)));
                    secondPreparation.timing.attack[selector][key] = rom2.at(0x30000+((attackPointer+key)&65535));
                    secondPreparation.timing.release[selector][key] = rom2.at(0x30000+((releasePointer+key)&65535));
                }
            }
            sc55::SoundData::ModulationRates modulationRates;
            importWords(modulationRates,0x7012);
            sc55::ModulationPreparationTables modulationPreparation;
            importWords(modulationPreparation.timing,0x7112);
            importWords(modulationPreparation.depths.secondEnvelope,0x7212);
            importWords(modulationPreparation.depths.pitch,0x7312);
            sc55::PanTable pan;
            std::copy_n(rom1.begin()+0x6c8f,pan.size(),pan.begin());
            sc55::PitchEnvelopeKeyCurves pitchTiming;
            for (unsigned selector = 0; selector < 16; ++selector)
                for (unsigned key = 0; key < 256; ++key)
                {
                    pitchTiming.attack[selector][key] = rom2.at(0x30000|uint16_t(rom2word(0x3dd42+2*selector)+key));
                    pitchTiming.release[selector][key] = rom2.at(0x30000|uint16_t(rom2word(0x3dd62+2*selector)+key));
                }
            const auto soundEncoded = sc55::SoundData::encode(
                std::span(rom2).subspan(0x10000,sc55::SoundData::patchBytes),velocityCurves,sampleBank,&envelopeTimes,&envelopeLevels,&envelopeKeys,&envelopeKeyLevels,&glideRates,&pitchKeys,&pitchEnvelope,&secondEnvelope,&secondPreparation,&modulationRates,&modulationPreparation,&pan,&pitchTiming,
                allSoundExportMode ? std::span<const uint8_t>(rom2).subspan(0x20000,sc55::SoundData::supplementalPatchBytes) : std::span<const uint8_t>{});
            if (!soundData.loadEncoded(soundEncoded)) throw std::runtime_error("Invalid melodic sound data");
            if (allSoundExportMode) verifyRhythmVelocity(emu.GetMCU(),soundData);
            if (!soundData.pitchTiming() || soundData.pitchTiming()->attack != pitchTiming.attack || soundData.pitchTiming()->release != pitchTiming.release)
                throw std::runtime_error("Pitch timing data changed during import");
            if (!soundData.pan() || *soundData.pan() != pan) throw std::runtime_error("Pan data changed during import");
            if (!soundData.modulationPreparation() || *soundData.modulationPreparation() != modulationPreparation) throw std::runtime_error("Modulation preparation data changed during import");
            if (!soundData.modulationRates() || *soundData.modulationRates() != modulationRates) throw std::runtime_error("Modulation rate data changed during import");
            if (!soundData.secondPreparation() || *soundData.secondPreparation() != secondPreparation) throw std::runtime_error("Second envelope preparation data changed during import");
            if (!soundData.secondEnvelope() || *soundData.secondEnvelope() != secondEnvelope) throw std::runtime_error("Second envelope data changed during import");
            if (!soundData.pitchEnvelope() || *soundData.pitchEnvelope() != pitchEnvelope) throw std::runtime_error("Pitch envelope data changed during import");
            if (!soundData.pitchKeys() || *soundData.pitchKeys() != pitchKeys) throw std::runtime_error("Pitch key data changed during import");
            if (!soundData.glideRates() || *soundData.glideRates() != glideRates) throw std::runtime_error("Glide rate data changed during import");
            if (!soundData.times() || *soundData.times() != envelopeTimes) throw std::runtime_error("Envelope time data changed during import");
            envelopeTimes = *soundData.times();
            for (unsigned index = 0; index < 224; ++index)
            {
                const auto& source = patches[int(index)];
                const auto& loaded = *soundData.patch(index);
                if (source.name != loaded.name
                    || !std::equal(std::begin(source.common),std::end(source.common),std::begin(loaded.common)))
                    throw std::runtime_error("Melodic patch common data changed during import");
                for (unsigned p = 0; p < 2; ++p)
                    if (!std::equal(std::begin(source.partial[p].raw),std::end(source.partial[p].raw),
                                    std::begin(loaded.partial[p].raw)))
                        throw std::runtime_error("Melodic partial data changed during import");
            }
            if (*soundData.curves() != velocityCurves) throw std::runtime_error("Velocity curves changed during import");
            velocityCurves = *soundData.curves();
            sampleBank = *soundData.samples();
            if (exportMode)
            {
                const auto& output = soundExportMode ? soundEncoded : encoded;
                FILE* file = std::fopen(argv[2],"wbx");
                if (!file) throw std::runtime_error("Cannot exclusively create sample bank output");
                const bool written = std::fwrite(output.data(),1,output.size(),file) == output.size();
                const int closed = std::fclose(file);
                if (!written || closed != 0) throw std::runtime_error("Sample bank write failed (partial file may remain)");
                std::printf("Exported %zu data-only bytes\n",output.size());
                return 0;
            }
        }
        emu.Reset();
        emu.GetMCU().button_pressed.store(0);
        emu.SetSampleCallback(sample, nullptr);
        sc55::MidiDecoder midiDecoder;
        sc55::ChannelControls controls;
        std::array<std::array<uint8_t,128>,16> receivedVelocities{};
        const auto send = [&](std::span<const uint8_t> data)
        {
            if (!midiMode && !controllerMode && !programMode && !outputControlMode) { emu.PostMIDI(data); return; }
            midiDecoder.push(data, [&](const sc55::MidiDecoder::Event& e)
            {
                using Kind = sc55::MidiDecoder::Kind;
                if (e.kind == Kind::message && (e.status & 0xf0) == 0x90 && e.second != 0)
                    receivedVelocities[e.status & 15][e.first] = e.second;
                if (controllerMode || programMode || outputControlMode) controls.apply(e);
                if (e.kind == Kind::message)
                {
                    const uint8_t packet[] {e.status, e.first, e.second};
                    emu.PostMIDI(std::span(packet, size_t(e.dataSize) + 1));
                }
                else if (e.kind == Kind::realtime || e.kind == Kind::sysexBegin || e.kind == Kind::sysexEnd)
                    emu.PostMIDI(e.status);
                else if (e.kind == Kind::sysexData)
                    emu.PostMIDI(e.first);
                // Native GS transaction handling will discard an abort; this
                // byte-forwarding reference receives the next status directly.
            });
        };
        trace = std::fopen(argv[2], "w");
        if (trace == nullptr) throw std::runtime_error("Cannot create trace");
        std::fprintf(trace, "phase,frame,cycle,pc_at_write,register,value\n");
        bool resetSent = false;
        int programUnderTest = -1, selectedPatch = -1;
        std::optional<sc55::SampleAddresses> expectedAddresses;
        uint16_t addressVoice = 0;
        unsigned addressChecks = 0;
        unsigned sampleControlChecks = 0;
        std::optional<uint16_t> expectedSample;
        std::optional<uint32_t> expectedDescriptor;
        uint16_t nativeSampleId = 0;
        unsigned descriptorChecks = 0;
        unsigned sampleSelectionChecks = 0;
        std::optional<sc55::TrackedKey> expectedPitch;
        std::optional<sc55::PartialSamplePlan> nativePartialSample;
        std::array<std::optional<sc55::PartialSamplePlan>,24> voiceSamplePlans;
        std::array<bool,24> voiceUnoffset{};
        std::array<uint8_t,24> voiceHistoryNibble{};
        std::array<std::optional<sc55::PartialEnvelopePlan>,24> voiceEnvelopePlans;
        std::optional<sc55::PartialEnvelopePlan> nativeEnvelope;
        std::optional<sc55::PatchVelocityPlan> completedPatchVelocity;
        unsigned envelopeStageChecks = 0;
        struct EnvelopeUpdate { uint16_t voice, level, word; sc55::EnvelopeProgress progress; };
        std::optional<EnvelopeUpdate> expectedEnvelopeUpdate;
        unsigned envelopeUpdateChecks = 0;
        std::optional<uint16_t> nativeEnvelopeDuration;
        unsigned liveDurationChecks = 0;
        unsigned livePitchInputChecks = 0;
        std::optional<sc55::EnvelopeStageState> expectedStage;
        uint16_t stageVoice = 0;
        unsigned stageChecks = 0;
        std::optional<sc55::EnvelopeStageState> expectedRelease;
        uint16_t releaseVoice = 0, releaseCode = 0;
        unsigned liveReleaseChecks = 0;
        std::optional<sc55::EnvelopeRunner> expectedEnvelopeStart;
        uint16_t startVoice = 0;
        uint16_t startStageCode = 0;
        unsigned liveStartChecks = 0;
        std::array<std::optional<sc55::EnvelopeRunner>,24> nativeEnvelopes;
        std::optional<unsigned> activatingEnvelope;
        unsigned liveActivationChecks = 0;
        std::optional<unsigned> updatingNativeEnvelope;
        uint16_t updatingNativeVoice = 0;
        unsigned persistentUpdateChecks = 0;
        std::optional<sc55::VoiceLinks> expectedDetachedLinks;
        unsigned liveDetachChecks = 0;
        std::optional<sc55::VoiceAllocator> expectedVoiceReturn;
        unsigned liveVoiceReturnChecks = 0;
        std::optional<sc55::VoiceAllocator> expectedVoiceTake, expectedVoiceAttach;
        std::optional<uint8_t> expectedTakenVoice;
        unsigned liveVoiceTakeChecks = 0, liveVoiceAttachChecks = 0;
        std::optional<sc55::VoiceAllocator> expectedGroupCreation;
        std::optional<sc55::VoiceAllocator::GroupAllocation> expectedGroupResult;
        unsigned expectedGroupVoiceCount = 0, liveGroupCreationChecks = 0;
        std::optional<sc55::VoiceAllocator> expectedAllocatorInitialization;
        unsigned liveAllocatorInitializationChecks = 0;
        std::optional<sc55::VoiceAllocator> expectedNoteRelease;
        unsigned liveNoteReleaseChecks = 0;
        std::optional<sc55::PartialKeyResolution> expectedResolution;
        uint8_t expectedOriginalNote = 0, expectedAdjustedKey = 0;
        unsigned pitchChecks = 0;
        std::optional<uint8_t> nativeInitialKey;
        uint8_t nativeSourceKey = 0;
        unsigned initialPart = 0;
        unsigned initialKeyChecks = 0;
        std::optional<sc55::PartialCandidates> expectedCandidates;
        unsigned candidateChecks = 0;
        std::optional<sc55::PatchVelocityPlan> expectedPatchVelocity;
        std::optional<sc55::VoiceControllerState> expectedControllers;
        uint16_t controllerVoiceBase = 0;
        unsigned liveControllerChecks = 0;
        unsigned deferredControllerEntries = 0;
        uint8_t nativeVelocityAccumulator = 0;
        uint8_t nativeIncomingVelocity = 0;
        uint16_t velocityPatchBase = 0;
        bool velocityPending = false;
        std::optional<sc55::PartialVelocityValues> expectedVelocity;
        unsigned velocityOperationChecks = 0;
        std::optional<sc55::HighNoteMapping> expectedHighNote;
        unsigned absentHighNotes = 0;
        std::set<uint64_t> patchProbes;
        ControlClockProbe controlClock;
        uint16_t outputControlParts = 0;
        unsigned outputControlChecks = 0;
        std::optional<sc55::MonoHeldKeys> monoKeys;
        std::optional<sc55::MonoHeldKeys::ReleaseDecision> monoDecision;
        unsigned monoPart = 0;
        std::array<unsigned,3> monoBranches{};
        bool monoPreparing = false;
        unsigned monoPreparationChecks = 0;
        std::optional<sc55::PatchVelocityPlan> monoVelocity;
        std::optional<std::array<sc55::PartialVoiceDispatch,2>> liveDispatch;
        std::array<sc55::PartialVoiceDispatch,2> observedDispatch{};
        unsigned dispatchChecks = 0, dualDispatchChecks = 0, sharedDispatchChecks = 0;
        const auto run = [&](double seconds)
        {
            const auto until = frames + static_cast<uint64_t>(seconds * PCM_GetOutputFrequency(emu.GetPCM()));
            const auto cycleLimit = emu.GetMCU().cycles + static_cast<uint64_t>((seconds + 1) * 100000000);
            while (frames < until)
            {
                const auto& before = emu.GetMCU();
                bool enteredControllerProbe = false;
                if ((monoMode || sampleMode) && !before.sleep && before.cp == 0)
                {
                    auto& cpu = emu.GetMCU();
                    // Observe the real delegates, without skipping sample preparation,
                    // restart, or installation. The isolated test stubs those calls.
                    if (before.pc == 0x1219)
                    {
                        if (liveDispatch) throw std::runtime_error("Nested live partial dispatch");
                        const auto base = uint16_t((MCU_Read(cpu,0xa1c2)<<8)|MCU_Read(cpu,0xa1c3));
                        const SC55Patch* patch = nullptr;
                        for (unsigned i = 0; i < patches.size(); ++i)
                            if (uint16_t(patches[i].rom_offset) == base) { patch = &patches[i]; break; }
                        if (!patch) throw std::runtime_error("Unknown live dispatch patch");
                        liveDispatch = sc55::PlanPartialVoiceDispatch(*patch,MCU_Read(cpu,0xa1b0),
                            {MCU_Read(cpu,0xa3d6),MCU_Read(cpu,0xa3d7)});
                        if (!liveDispatch) throw std::runtime_error("Invalid live partial dispatch inputs");
                        observedDispatch = {};
                    }
                    if (liveDispatch)
                    {
                        if (before.pc == 0x123a || before.pc == 0x12bb)
                            observedDispatch[before.pc == 0x123a ? 0 : 1].prepare = true;
                        if (before.pc == 0x129b || before.pc == 0x1323)
                        {
                            if (before.r[1] >= 24) throw std::runtime_error("Invalid live installation slot");
                            observedDispatch[before.pc == 0x129b ? 0 : 1].voice = uint8_t(before.r[1]);
                        }
                        if (before.pc == 0x132a)
                        {
                            for (unsigned i = 0; i < 2; ++i)
                            {
                                const auto& expected = (*liveDispatch)[i];
                                const auto& actual = observedDispatch[i];
                                if (expected.prepare != actual.prepare
                                    || (expected.voice < 128 ? expected.voice : 255) != actual.voice)
                                    throw std::runtime_error("Live partial dispatch differs from native");
                            }
                            ++dispatchChecks;
                            if (observedDispatch[0].prepare && observedDispatch[1].prepare)
                            {
                                ++dualDispatchChecks;
                                if (observedDispatch[0].voice < 24
                                    && observedDispatch[0].voice == observedDispatch[1].voice) ++sharedDispatchChecks;
                            }
                            liveDispatch.reset();
                        }
                    }
                }
                if (monoMode && !before.sleep && before.cp == 0)
                {
                    auto& cpu = emu.GetMCU();
                    if (before.pc == 0x0a64)
                    {
                        monoPart = before.r[3];
                        if (monoPart >= 16) throw std::runtime_error("Invalid live mono part");
                        monoKeys.emplace();
                        for (unsigned i = 0; i < 8; ++i)
                            monoKeys->words[i] = uint16_t(unsigned(MCU_Read(cpu,0x9f40+monoPart*16+2*i))*256
                                +MCU_Read(cpu,0x9f41+monoPart*16+2*i));
                        monoDecision = monoKeys->release(before.r[1],MCU_Read(cpu,0xa070+monoPart));
                        if (!monoDecision) throw std::runtime_error("Invalid live mono release key");
                    }
                    if (monoDecision && (before.pc == 0x0aec || before.pc == 0x0aab || before.pc == 0x0a74))
                    {
                        using Action = sc55::MonoHeldKeys::ReleaseDecision::Action;
                        const auto actual = before.pc == 0x0aec ? Action::unchangedVoice
                            : before.pc == 0x0aab ? Action::releaseGroup : Action::replaceKey;
                        if (monoDecision->action != actual || (actual == Action::replaceKey && monoDecision->replacement != before.r[1]))
                            throw std::runtime_error("Live mono decision differs from native");
                        for (unsigned i = 0; i < 8; ++i)
                            if (monoKeys->words[i] != unsigned(MCU_Read(cpu,0x9f40+monoPart*16+2*i))*256
                                +MCU_Read(cpu,0x9f41+monoPart*16+2*i))
                                throw std::runtime_error("Live mono held keys differ from native");
                        ++monoBranches[unsigned(actual)];
                        monoPreparing = actual == Action::replaceKey;
                        monoDecision.reset(); monoKeys.reset();
                    }
                    if (monoPreparing && (before.pc == 0x0fbe || before.pc == 0x0a8a))
                    {
                        if (monoPart != 1 || MCU_Read(cpu,0xa1b2) != 60 || before.r[4] != 0
                            || MCU_Read(cpu,0xa3d3) != 110 || MCU_Read(cpu,0xa080+monoPart) != 110
                            || MCU_Read(cpu,0xa1b7) != (before.pc == 0x0fbe ? 0 : 10))
                            throw std::runtime_error("Mono preparation probe inputs changed");
                        ++monoPreparationChecks;
                        if (before.pc == 0x0fbe)
                        {
                            const sc55::MonoHeldKeys::ReleaseDecision decision{
                                sc55::MonoHeldKeys::ReleaseDecision::Action::replaceKey,MCU_Read(cpu,0xa1b2)};
                            if (before.r[4] >= patches.size()) throw std::runtime_error("Invalid mono replacement tone");
                            monoVelocity = sc55::PrepareMonoReplacementVelocity(decision,patches[before.r[4]],
                                MCU_Read(cpu,0xa3d3),false,velocityCurves);
                            if (!monoVelocity) throw std::runtime_error("Native mono velocity preparation rejected live input");
                        }
                        else
                        {
                            if (!monoVelocity || monoVelocity->accumulator != MCU_Read(cpu,0xa1b7)
                                || monoVelocity->candidates.flags != MCU_Read(cpu,0xa1b0)
                                || monoVelocity->candidates.count != MCU_Read(cpu,0xa3d4))
                                throw std::runtime_error("Native mono velocity plan differs from H8");
                            for (unsigned i = 0; i < 2; ++i)
                                if (const auto& values = monoVelocity->partials[i])
                                    if (values->amplitude != MCU_Read(cpu,0xa1b9+i)
                                        || values->secondary != MCU_Read(cpu,0xa1bc+i))
                                        throw std::runtime_error("Native mono partial velocity differs from H8");
                            monoVelocity.reset();
                        }
                        std::printf("mono-prepare pc=%04x part=%u key=%u tone=%u velocity=%u savedVelocity=%u accumulator=%u\n",
                            before.pc,monoPart,MCU_Read(cpu,0xa1b2),before.r[4],MCU_Read(cpu,0xa3d3),
                            MCU_Read(cpu,0xa080+monoPart),MCU_Read(cpu,0xa1b7));
                        if (before.pc == 0x0a8a) monoPreparing = false;
                    }
                }
                if (outputControlMode && !before.sleep && before.cp == 0 && before.pc == 0x2fcc)
                {
                    auto& cpu = emu.GetMCU();
                    const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                    const auto pointer = word(uint16_t(before.r[0]+0x2e));
                    const auto slot = word(uint16_t(before.r[0]-2));
                    if (slot >= 24) throw std::runtime_error("Invalid output-control voice slot");
                    const unsigned part = MCU_Read(cpu,0xc8e4+slot);
                    if (part >= 16) throw std::runtime_error("Invalid output-control part");
                    const unsigned channel = part == 0 ? 9 : part <= 9 ? part-1 : part;
                    sc55::LevelInputs level; sc55::SpatialInputs spatial;
                    sc55::ApplyChannelOutputControls(controls.channel(channel),level,spatial);
                    if (level.velocity != MCU_Read(cpu,pointer+8) || level.expression != MCU_Read(cpu,0xab36+part)
                        || spatial.pan != MCU_Read(cpu,pointer+9) || spatial.reverb != MCU_Read(cpu,pointer+14)
                        || spatial.chorus != MCU_Read(cpu,pointer+15))
                        throw std::runtime_error("MIDI-to-voice output-control mapping differs from H8");
                    outputControlParts |= uint16_t(1u<<part); ++outputControlChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 4 && before.pc == 0x04b9 && !expectedAllocatorInitialization)
                {
                    expectedAllocatorInitialization.emplace();
                    visitAllocatorBytes(*expectedAllocatorInitialization,[&](unsigned address,uint8_t& value) {
                        value = MCU_Read(emu.GetMCU(),address);
                    });
                    const unsigned voices = ((MCU_Read(emu.GetMCU(),0xa3ca)<<8)|MCU_Read(emu.GetMCU(),0xa3cb))+1;
                    if (before.r[3] != 15 || !expectedAllocatorInitialization->initializeTables(voices,unsigned(before.r[2])+1))
                        throw std::runtime_error("Invalid live allocator initialization");
                }
                if (sampleMode && !before.sleep && before.cp == 4 && before.pc == 0x0569 && expectedAllocatorInitialization)
                {
                    visitAllocatorBytes(*expectedAllocatorInitialization,[&](unsigned address,uint8_t value) {
                        if (value != MCU_Read(emu.GetMCU(),address)) throw std::runtime_error("Live allocator initialization differs");
                    });
                    expectedAllocatorInitialization.reset(); ++liveAllocatorInitializationChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0)
                {
                    if (before.pc == 0x5c20)
                    {
                        if (expectedControllers || before.r[1] >= 24)
                            throw std::runtime_error("Invalid live controller entry: pending="+std::to_string(bool(expectedControllers))
                                +" slot="+std::to_string(before.r[1])+" voice="+std::to_string(before.r[0]));
                        auto& cpu = emu.GetMCU();
                        const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                        const unsigned part = MCU_Read(cpu,0xc8e4+before.r[1]);
                        const unsigned key = MCU_Read(cpu,0xc8fc+before.r[1]);
                        if (part >= 16) throw std::runtime_error("Invalid live controller part");
                        const auto partBase = word(0x74a4+part*2);
                        sc55::VoiceControllerInputs input;
                        input.keyValue = MCU_Read(cpu,0x9740+part*128+key);
                        for (unsigned i = 0; i < 11; ++i)
                        {
                            input.sensitivity[i] = MCU_Read(cpu,partBase+0x4c+i+(i >= 3));
                            for (unsigned source = 0; source < 5; ++source)
                                input.contributions[source][i] = word(0x9060+source*0x160+i*0x20+part*2);
                        }
                        expectedControllers = sc55::PrepareVoiceControllers(input);
                        controllerVoiceBase = before.r[0];
                        enteredControllerProbe = true;
                    }
                    if (before.pc == 0x5ff4 && expectedControllers)
                    {
                        const auto& expected = *expectedControllers;
                        const std::array<int,11> offsets{0x86,0x88,0x8a,-0x72,0x90,0x8c,0x8e,-0x50,0x92,0x94,0x96};
                        const std::array<uint16_t,11> values{expected.pitchOffset,expected.envelopeOffset,expected.levelBias,
                            expected.rateModifiers[0],expected.pitchDepths[0],expected.envelopeDepths[0],expected.levelDepths[0],
                            expected.rateModifiers[1],expected.pitchDepths[1],expected.envelopeDepths[1],expected.levelDepths[1]};
                        for (unsigned i = 0; i < 11; ++i)
                        {
                            const auto a = uint16_t(controllerVoiceBase+offsets[i]);
                            if (unsigned(MCU_Read(emu.GetMCU(),a))*256+MCU_Read(emu.GetMCU(),uint16_t(a+1)) != values[i])
                                throw std::runtime_error("Live controller preparation differs at field "+std::to_string(i));
                        }
                        expectedControllers.reset(); ++liveControllerChecks;
                    }
                    const auto snapshot = [&](auto& state) {
                        state.emplace();
                        visitAllocatorBytes(*state,[&](unsigned address,uint8_t& value) { value = MCU_Read(emu.GetMCU(),address); });
                    };
                    const auto compare = [&](auto& state) {
                        visitAllocatorBytes(*state,[&](unsigned address,uint8_t value) {
                            if (value != MCU_Read(emu.GetMCU(),address))
                                throw std::runtime_error("Live voice allocation mismatch at " + std::to_string(address));
                        });
                        state.reset();
                    };
                    if (before.pc == 0x1bab && !expectedGroupCreation)
                    {
                        snapshot(expectedGroupCreation);
                        const auto read = [&](unsigned address) { return MCU_Read(emu.GetMCU(),address); };
                        expectedGroupVoiceCount = read(0xa3d4);
                        expectedGroupResult = expectedGroupCreation->createGroup({read(0xa3d0),read(0xa3d1),read(0xa3d2),read(0xa1bf),uint8_t(expectedGroupVoiceCount)});
                        if (!expectedGroupResult) throw std::runtime_error("Invalid live group creation");
                    }
                    if (before.pc == 0x155a && !expectedNoteRelease)
                    {
                        snapshot(expectedNoteRelease);
                        const auto part = uint8_t(before.r[3]);
                        std::array<uint8_t,16> retained;
                        for (unsigned i = 0; i < 16; ++i) retained[i] = MCU_Read(emu.GetMCU(),0xa090+16*part+i);
                        if (!expectedNoteRelease->requestNoteRelease(part,uint8_t(before.r[1]),uint8_t(before.r[0]),retained))
                            throw std::runtime_error("Invalid live note release inputs");
                    }
                    if (before.pc == 0x1597 && expectedNoteRelease)
                    {
                        compare(expectedNoteRelease); ++liveNoteReleaseChecks;
                    }
                    if (before.pc == 0x1c3f && expectedGroupCreation)
                    {
                        if (expectedGroupResult->group != MCU_Read(emu.GetMCU(),0xa3d5))
                            throw std::runtime_error("Live group selection differs");
                        for (unsigned i = 0; i <= expectedGroupVoiceCount; ++i)
                            if (expectedGroupResult->voices[i] != MCU_Read(emu.GetMCU(),0xa3d6+i))
                                throw std::runtime_error("Live group voice order differs");
                        compare(expectedGroupCreation); expectedGroupResult.reset(); ++liveGroupCreationChecks;
                    }
                    if (before.pc == 0x1ca5 && !expectedVoiceTake)
                    {
                        snapshot(expectedVoiceTake);
                        expectedTakenVoice = expectedVoiceTake->takeFreeVoice();
                        if (!expectedTakenVoice) throw std::runtime_error("Invalid live free voice selection");
                    }
                    if (before.pc == 0x1cbc && expectedVoiceTake)
                    {
                        if (*expectedTakenVoice != before.r[1]) throw std::runtime_error("Live free voice selection differs");
                        compare(expectedVoiceTake); expectedTakenVoice.reset(); ++liveVoiceTakeChecks;
                    }
                    if (before.pc == 0x1c40 && !expectedVoiceAttach)
                    {
                        snapshot(expectedVoiceAttach);
                        if (!expectedVoiceAttach->attachVoice(before.r[1],before.r[2],before.r[3]))
                            throw std::runtime_error("Invalid live voice attach");
                    }
                    if (before.pc == 0x1ca4 && expectedVoiceAttach)
                    { compare(expectedVoiceAttach); ++liveVoiceAttachChecks; }
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x1cbd && !expectedVoiceReturn)
                {
                    expectedVoiceReturn.emplace();
                    visitAllocatorBytes(*expectedVoiceReturn,[&](unsigned address,uint8_t& value) {
                        value = MCU_Read(emu.GetMCU(),address);
                    });
                    if (!expectedVoiceReturn->returnVoice(before.r[1]))
                        throw std::runtime_error("Invalid live voice return state");
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x1d54 && expectedVoiceReturn)
                {
                    visitAllocatorBytes(*expectedVoiceReturn,[&](unsigned address,uint8_t value) {
                        if (value != MCU_Read(emu.GetMCU(),address))
                            throw std::runtime_error("Live voice return mismatch at " + std::to_string(address));
                    });
                    expectedVoiceReturn.reset(); ++liveVoiceReturnChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x339b)
                {
                    expectedDetachedLinks.emplace();
                    for (unsigned i = 0; i < sc55::VoiceLinks::count; ++i)
                    {
                        expectedDetachedLinks->first[i] = MCU_Read(emu.GetMCU(),0xcac4+i);
                        expectedDetachedLinks->second[i] = MCU_Read(emu.GetMCU(),0xcadc+i);
                    }
                    if (!expectedDetachedLinks->detach(before.r[1])) throw std::runtime_error("Invalid live voice links");
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x33c9)
                {
                    if (!expectedDetachedLinks) throw std::runtime_error("Missing voice detach plan");
                    for (unsigned i = 0; i < sc55::VoiceLinks::count; ++i)
                        if (expectedDetachedLinks->first[i] != MCU_Read(emu.GetMCU(),0xcac4+i)
                            || expectedDetachedLinks->second[i] != MCU_Read(emu.GetMCU(),0xcadc+i))
                            throw std::runtime_error("Native live voice detach differs");
                    expectedDetachedLinks.reset(); ++liveDetachChecks;
                    unsigned count = 0;
                    if (before.r[1] >= 32 || !sc55::WriteEnvelopeTermination(uint8_t(before.r[1]),[&](uint8_t address,uint8_t value) {
                        if (count >= expectedTermination.size()) throw std::runtime_error("Oversized termination transaction");
                        expectedTermination[count++] = {address,value};
                    })) throw std::runtime_error("Invalid termination channel");
                    if (count != expectedTermination.size()) throw std::runtime_error("Short termination transaction");
                    terminationByte = 0;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x485c)
                {
                    auto& cpu = emu.GetMCU();
                    const auto byte = [&](unsigned a) { return MCU_Read(cpu,a); };
                    const auto word = [&](unsigned a) { return uint16_t((byte(a)<<8)|byte(a+1)); };
                    const unsigned voice = before.r[0];
                    const unsigned patchAt = ((unsigned(byte(voice+0x98))<<16)|word(voice+0x9c)) & before.rom2_mask;
                    const unsigned partialAt = ((unsigned(byte(voice+0x99))<<16)|word(voice+0x9e)) & before.rom2_mask;
                    const unsigned sampleAt = ((unsigned(byte(voice+0x9a))<<16)|word(voice+0xa0)) & before.rom2_mask;
                    if (patchAt < 0x10000 || (patchAt-0x10000)%SC55Patch::SIZE != 0)
                        throw std::runtime_error("Pitch input patch alignment differs");
                    const auto* patch = soundData.patch((patchAt-0x10000)/SC55Patch::SIZE);
                    const unsigned partialIndex = partialAt == patchAt+0x20 ? 0 : partialAt == patchAt+0x7c ? 1 : 2;
                    const unsigned sampleBankIndex = sampleAt>>16;
                    const unsigned sampleLow = sampleAt&0xffff;
                    if (!patch || partialIndex >= 2 || (sampleBankIndex != 1 && sampleBankIndex != 2) || sampleLow < 0xdec0 || (sampleLow-0xdec0)%16)
                        throw std::runtime_error("Pitch input sample mapping differs");
                    const uint16_t sampleId = uint16_t((sampleBankIndex == 2 ? 532 : 0)+(sampleLow-0xdec0)/16);
                    sc55::PartialSamplePlan plan{}; plan.sampleId = sampleId;
                    const unsigned slot = word(voice-2);
                    const auto input = sc55::PreparePartialPitchInputs(*patch,partialIndex,plan,*soundData.samples(),
                        byte(0xc98c+before.r[1]),word(0xc9a4+2*before.r[1]),byte(voice-0x3b),byte(0xc974+slot));
                    if (!input || input->sampleKey != byte(sampleAt+11) || input->sampleTune != word(sampleAt+12)
                        || input->alternateTune != word(sampleAt+14) || input->keyTable != byte(patchAt+19)
                        || input->fine != byte(partialAt+11) || input->randomDepth != byte(partialAt+12))
                        throw std::runtime_error("Owned pitch inputs differ from live firmware");
                    ++livePitchInputChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x2c5d)
                {
                    const auto byte = [&](int d) { return MCU_Read(emu.GetMCU(),uint16_t(before.r[0]+d)); };
                    const auto word = [&](int d) { return uint16_t((byte(d)<<8)|byte(d+1)); };
                    const unsigned offset = ((unsigned(byte(0x99))<<16)|word(0x9e)) & before.rom2_mask;
                    if (offset < 0x10000) throw std::runtime_error("Envelope partial outside melodic records");
                    const unsigned index = (offset-0x10000)/SC55Patch::SIZE, within = (offset-0x10000)%SC55Patch::SIZE;
                    const auto* patch = soundData.patch(index);
                    if (!patch || (within != 0x20 && within != 0x7c)) throw std::runtime_error("Envelope partial not aligned");
                    const unsigned slot = word(-2);
                    const auto key = MCU_Read(emu.GetMCU(),0xc8fc+slot), amplitude = MCU_Read(emu.GetMCU(),0xc944+slot);
                    const auto record = MCU_Read(emu.GetMCU(),(unsigned(byte(0x9a))<<16)|word(0xa0));
                    const auto control = MCU_Read(emu.GetMCU(),uint16_t(word(0x2e)+20));
                    // This initialization does not install the allocator's
                    // active stage. Preserve/check its raw value separately;
                    // a neutral native stage is only used to prepare fields.
                    startStageCode = word(0);
                    expectedEnvelopeStart = sc55::EnvelopeRunner::fromKey(patch->partial[within == 0x20 ? 0 : 1],
                        {key,amplitude,record,patch->common[0],sc55::EnvelopeStage::attack1,control},
                        *soundData.levels(),*soundData.keyLevels(),*soundData.keys(),*soundData.times());
                    if (!expectedEnvelopeStart) throw std::runtime_error("Unsupported live envelope initialization");
                    startVoice = before.r[0];
                }
                if (sampleMode && expectedEnvelopeStart && !before.sleep && before.cp == 0 && before.pc == 0x2fcc)
                {
                    const auto byte = [&](int d) { return MCU_Read(emu.GetMCU(),uint16_t(startVoice+d)); };
                    const auto word = [&](int d) { return uint16_t((byte(d)<<8)|byte(d+1)); };
                    const auto& state = expectedEnvelopeStart->state(); const auto& setup = expectedEnvelopeStart->setup();
                    if (word(0) != startStageCode || word(8) != state.segment.progress.position || word(18) != state.segment.progress.deferredTicks
                        || word(28) != state.level || word(30) != state.pcmWord || word(14) != state.delayAccumulator
                        || word(16) != setup.delayIncrement || word(-34) != setup.keyScale || word(-32) != setup.releaseKeyScale)
                        throw std::runtime_error("Composed live envelope initialization differs");
                    for (unsigned i = 0; i < 4; ++i)
                        if (byte(0x61+i) != setup.targets[i]) throw std::runtime_error("Composed live envelope targets differ");
                    const auto slot = word(-2);
                    if (slot >= nativeEnvelopes.size()) throw std::runtime_error("Invalid envelope slot");
                    nativeEnvelopes[slot] = expectedEnvelopeStart;
                    expectedEnvelopeStart.reset(); ++liveStartChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x57ca)
                {
                    const auto slot = unsigned((MCU_Read(emu.GetMCU(),uint16_t(before.r[0]-2))<<8)
                        | MCU_Read(emu.GetMCU(),uint16_t(before.r[0]-1)));
                    if (slot >= nativeEnvelopes.size() || !nativeEnvelopes[slot]) throw std::runtime_error("Missing native activation state");
                    nativeEnvelopes[slot]->activateNewVoice();
                    activatingEnvelope = slot;
                }
                if (sampleMode && activatingEnvelope && !before.sleep && before.cp == 0 && before.pc == 0x57f5)
                {
                    const auto byte = [&](int d) { return MCU_Read(emu.GetMCU(),uint16_t(before.r[0]+d)); };
                    const auto word = [&](int d) { return uint16_t((byte(d)<<8)|byte(d+1)); };
                    const auto& state = nativeEnvelopes[*activatingEnvelope]->state();
                    if (word(-2) != *activatingEnvelope || word(0) != unsigned(state.segment.stage)*2
                        || word(8) != state.segment.progress.position || word(18) != state.segment.progress.deferredTicks
                        || word(28) != state.level || word(30) != state.pcmWord || word(14) != state.delayAccumulator)
                        throw std::runtime_error("Native live envelope activation differs");
                    activatingEnvelope.reset(); ++liveActivationChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x31c7)
                {
                    const auto slot = unsigned((MCU_Read(emu.GetMCU(),uint16_t(before.r[0]-2))<<8)
                        | MCU_Read(emu.GetMCU(),uint16_t(before.r[0]-1)));
                    if (slot >= nativeEnvelopes.size() || !nativeEnvelopes[slot]) throw std::runtime_error("Missing native PCM sync plan");
                    expectedEnvelopeSync = sc55::PrepareEnvelopePcmSync(*nativeEnvelopes[slot]); syncByte = 0;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x31d7)
                {
                    const auto word = [&](int d) { const auto at = uint16_t(before.r[0]+d);
                        return uint16_t((MCU_Read(emu.GetMCU(),at)<<8)|MCU_Read(emu.GetMCU(),uint16_t(at+1))); };
                    const auto slot = word(-2);
                    if (slot >= nativeEnvelopes.size() || !nativeEnvelopes[slot]) throw std::runtime_error("Missing native PCM sync state");
                    nativeEnvelopes[slot]->synchronizePcmLevel(sc55::DecodeEnvelopePcmLevel(before.r[5]));
                }
                if (sampleMode && updatingNativeEnvelope && !before.sleep && before.cp == 0
                    && (before.pc == 0x36a7 || before.pc == 0x36ad || before.pc == 0x36da
                        || before.pc == 0x348b || before.pc == 0x3471 || before.pc == 0x3472))
                {
                    const auto byte = [&](int d) { return MCU_Read(emu.GetMCU(),uint16_t(updatingNativeVoice+d)); };
                    const auto word = [&](int d) { return uint16_t((byte(d)<<8)|byte(d+1)); };
                    const auto& state = nativeEnvelopes[*updatingNativeEnvelope]->state();
                    const unsigned stage = before.pc == 0x3472 ? unsigned(sc55::EnvelopeStage::finished) : word(0)/2;
                    if (stage != unsigned(state.segment.stage) || word(8) != state.segment.progress.position
                        || word(18) != state.segment.progress.deferredTicks || word(28) != state.level || word(30) != state.pcmWord
                        || word(14) != state.delayAccumulator || byte(96) != state.segment.start || byte(97) != state.segment.target
                        || byte(79) != state.segment.parameter.value || byte(-8) != state.segment.parameter.flag)
                    {
                        std::fprintf(stderr,"Persistent update mismatch pc=%04x slot=%u stages=%u/%u progress=%u/%u level=%u/%u word=%04x/%04x\n",
                            before.pc,*updatingNativeEnvelope,unsigned(state.segment.stage),stage,state.segment.progress.position,word(8),state.level,word(28),state.pcmWord,word(30));
                        throw std::runtime_error("Persistent live envelope differs");
                    }
                    updatingNativeEnvelope.reset(); ++persistentUpdateChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x3220)
                {
                    releaseVoice = before.r[0];
                    const auto byte = [&](int d) { return MCU_Read(emu.GetMCU(),uint16_t(releaseVoice+d)); };
                    const auto word = [&](int d) { return uint16_t((byte(d)<<8)|byte(d+1)); };
                    const auto code = word(0);
                    const auto slot = word(-2);
                    if (!expectedRelease)
                    {
                        if (slot >= nativeEnvelopes.size() || !nativeEnvelopes[slot]) throw std::runtime_error("Missing native release state");
                        auto& native = *nativeEnvelopes[slot];
                        native.release(native.state().level);
                    }
                    const auto stage = code >= 14 ? sc55::EnvelopeStage::finished : static_cast<sc55::EnvelopeStage>(code/2);
                    if (code < 14 && (code & 1)) throw std::runtime_error("Invalid release stage");
                    expectedRelease = sc55::ReleaseEnvelope({stage,{word(8),word(18)},{byte(79),byte(-8)},byte(96),byte(97)},
                        word(28),{byte(83),byte(-4)},word(16));
                    releaseCode = code >= 12 ? code : expectedRelease->stage == sc55::EnvelopeStage::finished ? 22
                        : uint16_t(unsigned(expectedRelease->stage)*2);
                }
                if (sampleMode && expectedRelease && !before.sleep && before.cp == 0
                    && (before.pc == 0x328b || before.pc == 0x32f0 || before.pc == 0x3363))
                {
                    const auto byte = [&](int d) { return MCU_Read(emu.GetMCU(),uint16_t(releaseVoice+d)); };
                    const auto word = [&](int d) { return uint16_t((byte(d)<<8)|byte(d+1)); };
                    const auto& expected = *expectedRelease;
                    if (before.r[0] != releaseVoice || word(0) != releaseCode || word(8) != expected.progress.position
                        || word(18) != expected.progress.deferredTicks || byte(79) != expected.parameter.value
                        || byte(-8) != expected.parameter.flag || byte(96) != expected.start || byte(97) != expected.target)
                        throw std::runtime_error("Native live envelope release differs");
                    expectedRelease.reset(); ++liveReleaseChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x33f4)
                {
                    stageVoice = before.r[0];
                    const auto byte = [&](int d) { return MCU_Read(emu.GetMCU(),uint16_t(stageVoice+d)); };
                    const auto word = [&](int d) { return uint16_t((byte(d)<<8)|byte(d+1)); };
                    const auto code = word(0);
                    if (!updatingNativeEnvelope)
                    {
                        const auto slot = word(-2);
                        if (slot >= nativeEnvelopes.size() || !nativeEnvelopes[slot]) throw std::runtime_error("Missing persistent native envelope");
                        const auto part = word(46);
                        const auto control = [&](unsigned d) { return MCU_Read(emu.GetMCU(),uint16_t(part+d)); };
                        const auto ticks = uint16_t((MCU_Read(emu.GetMCU(),0xac5a)<<8)|MCU_Read(emu.GetMCU(),0xac5b));
                        if (!nativeEnvelopes[slot]->tick(ticks,{control(20),control(21),control(22)},*soundData.times()))
                            throw std::runtime_error("Persistent native tick rejected");
                        updatingNativeEnvelope = slot; updatingNativeVoice = before.r[0];
                    }
                    if (code > 12 || (code & 1)) throw std::runtime_error("Unsupported envelope state entry");
                    std::array<SC55ExpandedParameter,5> parameters;
                    std::array<uint8_t,4> targets;
                    for (unsigned i = 0; i < 5; ++i) parameters[i] = {byte(79+i),byte(int(i)-8)};
                    for (unsigned i = 0; i < 4; ++i) targets[i] = byte(97+i);
                    expectedStage = sc55::AdvanceEnvelopeStage({static_cast<sc55::EnvelopeStage>(code/2),
                        {word(8),word(18)},parameters[0],byte(96),targets[0]},parameters,targets);
                }
                if (sampleMode && expectedStage && !before.sleep && before.cp == 0
                    && (before.pc == 0x3445 || before.pc == 0x3472))
                {
                    const auto byte = [&](int d) { return MCU_Read(emu.GetMCU(),uint16_t(stageVoice+d)); };
                    const auto word = [&](int d) { return uint16_t((byte(d)<<8)|byte(d+1)); };
                    const auto& expected = *expectedStage;
                    const unsigned code = expected.stage == sc55::EnvelopeStage::finished ? 22 : unsigned(expected.stage)*2;
                    if (before.r[0] != stageVoice || word(0) != code || word(8) != expected.progress.position
                        || word(18) != expected.progress.deferredTicks || byte(79) != expected.parameter.value
                        || byte(-8) != expected.parameter.flag || byte(96) != expected.start || byte(97) != expected.target)
                        throw std::runtime_error("Native live envelope transition differs");
                    expectedStage.reset(); ++stageChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0
                    && (before.pc == 0x348c || before.pc == 0x34e2 || before.pc == 0x353c))
                {
                    const auto byte = [&](int d) { return MCU_Read(emu.GetMCU(),uint16_t(before.r[0]+d)); };
                    const auto word = [&](int d) { return uint16_t((byte(d)<<8)|byte(d+1)); };
                    const auto voice = word(-2);
                    if (voice >= voiceEnvelopePlans.size() || !voiceEnvelopePlans[voice])
                        throw std::runtime_error("Missing envelope duration plan");
                    const auto& plan = *voiceEnvelopePlans[voice];
                    const unsigned controllerOffset = before.pc == 0x348c ? 20 : before.pc == 0x34e2 ? 21 : 22;
                    const auto control = MCU_Read(emu.GetMCU(),uint16_t(word(46)+controllerOffset));
                    nativeEnvelopeDuration = sc55::PrepareEnvelopeDuration(byte(79),control,
                        word(before.pc == 0x353c ? -32 : -34),
                        before.pc == 0x348c ? plan.velocityScale1 : plan.velocityScale2,envelopeTimes);
                    if (!nativeEnvelopeDuration) throw std::runtime_error("Unsupported envelope duration input");
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x358e)
                {
                    if (!nativeEnvelopeDuration || *nativeEnvelopeDuration != before.r[6])
                        throw std::runtime_error("Native live envelope duration differs");
                    ++liveDurationChecks;
                    const auto byte = [&](int d) { return MCU_Read(emu.GetMCU(),uint16_t(before.r[0]+d)); };
                    const auto word = [&](int d) { return uint16_t((byte(d)<<8)|byte(d+1)); };
                    const auto ticks = uint16_t((MCU_Read(emu.GetMCU(),0xac5a)<<8)|MCU_Read(emu.GetMCU(),0xac5b));
                    const auto step = sc55::StepEnvelopeSegment(*nativeEnvelopeDuration,ticks,{word(8),word(18)},
                        byte(0x60),byte(0x61),byte(-8) != 0,word(0x1c));
                    if (!step) throw std::runtime_error("Unsupported envelope update");
                    expectedEnvelopeUpdate = EnvelopeUpdate{before.r[0],step->level,step->pcmWord,step->progress};
                }
                if (sampleMode && expectedEnvelopeUpdate && !before.sleep && before.cp == 0 && before.pc == 0x365d)
                {
                    const auto& expected = *expectedEnvelopeUpdate;
                    const auto word = [&](int d) { const auto a = uint16_t(expected.voice+d);
                        return uint16_t((MCU_Read(emu.GetMCU(),a)<<8)|MCU_Read(emu.GetMCU(),uint16_t(a+1))); };
                    if (before.r[0] != expected.voice || before.r[2] != expected.level
                        || word(8) != expected.progress.position || word(18) != expected.progress.deferredTicks)
                        throw std::runtime_error("Composed envelope progress/level differs");
                }
                if (sampleMode && expectedEnvelopeUpdate && !before.sleep && before.cp == 0
                    && (before.pc == 0x36a7 || before.pc == 0x36ad || before.pc == 0x36da))
                {
                    const auto& expected = *expectedEnvelopeUpdate;
                    const auto word = [&](int d) { const auto a = uint16_t(expected.voice+d);
                        return uint16_t((MCU_Read(emu.GetMCU(),a)<<8)|MCU_Read(emu.GetMCU(),uint16_t(a+1))); };
                    if (before.r[0] != expected.voice || word(0x1c) != expected.level || word(0x1e) != expected.word
                        || word(8) != expected.progress.position || word(18) != expected.progress.deferredTicks)
                        throw std::runtime_error("Composed envelope output differs");
                    expectedEnvelopeUpdate.reset(); ++envelopeUpdateChecks;
                }
                // The v1.21 receive ring's overflow path injects a controller
                // reset. Reject it rather than comparing against lost input.
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x0601)
                    throw std::runtime_error("Reference firmware MIDI receive ring overflow");
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x0bc1)
                {
                    nativeVelocityAccumulator = 0; // same note-entry reset as 00:0bcd
                    const unsigned part = before.r[3];
                    if (part >= 16 || uint8_t(before.r[1]) >= 128) throw std::runtime_error("Invalid note entry");
                    const unsigned channel = part == 0 ? 9 : part < 10 ? part-1 : part;
                    nativeIncomingVelocity = receivedVelocities[channel][uint8_t(before.r[1])];
                    if (nativeIncomingVelocity != uint8_t(before.r[2])) throw std::runtime_error("Received MIDI velocity differs at note entry");
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x1034)
                {
                    const auto offset = uint16_t(before.r[5]-velocityPatchBase);
                    if (!expectedPatchVelocity || (offset != 32 && offset != 124))
                        throw std::runtime_error("Missing whole-patch velocity plan");
                    expectedVelocity = expectedPatchVelocity->partials[offset == 32 ? 0 : 1];
                    velocityPending = true;
                }
                if (sampleMode && velocityPending && !before.sleep && before.cp == 0 && before.pc == 0x1119)
                {
                    if (bool(expectedVelocity) != (uint8_t(before.r[3]) == 0))
                        throw std::runtime_error("Native velocity acceptance differs");
                    if (expectedVelocity && (expectedVelocity->accumulator != MCU_Read(emu.GetMCU(),0xa1b7)
                        || expectedVelocity->amplitude != MCU_Read(emu.GetMCU(),0xa1b8)
                        || expectedVelocity->secondary != MCU_Read(emu.GetMCU(),0xa1bb)))
                        throw std::runtime_error("Native velocity values differ");
                    velocityPending = false;
                    ++velocityOperationChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x0fbe)
                {
                    if (before.r[4] >= patches.size()) throw std::runtime_error("Candidate tone outside imported patches");
                    const auto& patch = *soundData.patch(before.r[4]);
                    const auto part = MCU_Read(emu.GetMCU(),0xa3d0);
                    if (part >= 16) throw std::runtime_error("Invalid velocity part");
                    const unsigned channel = part == 0 ? 9 : part < 10 ? part-1 : part;
                    expectedPatchVelocity = sc55::PreparePatchVelocity(patch.common[6],nativeIncomingVelocity,
                        patch.partial[0].raw,patch.partial[1].raw,nativeVelocityAccumulator,
                        controls.channel(channel).softPedal,velocityCurves);
                    velocityPatchBase = uint16_t(patch.rom_offset);
                    expectedCandidates = expectedPatchVelocity->candidates;
                }
                if (sampleMode && expectedCandidates && !before.sleep && before.cp == 0 && before.pc == 0x1033)
                {
                    if (MCU_Read(emu.GetMCU(),0xa1b0) != expectedCandidates->flags
                        || MCU_Read(emu.GetMCU(),0xa3d4) != expectedCandidates->count)
                        throw std::runtime_error("Native partial candidates differ from firmware");
                    if (MCU_Read(emu.GetMCU(),0xa1b7) != expectedPatchVelocity->accumulator)
                        throw std::runtime_error("Native patch accumulator differs from firmware");
                    for (unsigned i = 0; i < 2; ++i)
                        if (const auto& values = expectedPatchVelocity->partials[i])
                            if (values->amplitude != MCU_Read(emu.GetMCU(),0xa1b9+i)
                                || values->secondary != MCU_Read(emu.GetMCU(),0xa1bc+i))
                                throw std::runtime_error("Native per-partial velocity outputs differ");
                    nativeVelocityAccumulator = expectedPatchVelocity->accumulator;
                    completedPatchVelocity = expectedPatchVelocity;
                    expectedPatchVelocity.reset();
                    expectedCandidates.reset();
                    ++candidateChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x0ccd)
                {
                    const auto tone = sc55::ResolveV121MelodicPreset(0,controls.channel(0).program);
                    if (!tone || before.r[4] != *tone) throw std::runtime_error("High-note tone selection differs");
                    expectedHighNote = sc55::MapHighNote(uint8_t(before.r[1]),soundData.patch(*tone)->common);
                    if (!expectedHighNote) throw std::runtime_error("Unexpected high-note input");
                }
                if (sampleMode && expectedHighNote && !before.sleep && before.cp == 0 && before.pc == 0x0ce8)
                {
                    if (before.r[4] != expectedHighNote->tone || MCU_Read(emu.GetMCU(),0xa1b2) != expectedHighNote->note)
                        throw std::runtime_error("High-note mapping differs from firmware");
                    if (expectedHighNote->tone & 0x8000) ++absentHighNotes;
                    expectedHighNote.reset();
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x11d0)
                {
                    auto& cpu = emu.GetMCU();
                    const uint16_t part = uint16_t((MCU_Read(cpu,0xa1c0) << 8) | MCU_Read(cpu,0xa1c1));
                    nativeSourceKey = MCU_Read(cpu,0xa1b2);
                    const unsigned partIndex = before.r[3];
                    initialPart = partIndex;
                    if (partIndex >= 16) throw std::runtime_error("Invalid part index at note entry");
                    const unsigned channel = partIndex == 0 ? 9 : partIndex < 10 ? partIndex-1 : partIndex;
                    nativeInitialKey = sc55::TransposeMasterKey(sc55::TransposePartKey(nativeSourceKey,
                        MCU_Read(cpu,uint16_t(part+6)), uint8_t(controls.channel(channel).coarseTuning)),
                        MCU_Read(cpu,0x8005));
                }
                // Firmware commits a prepared partial to a logical voice here;
                // initialization may happen after another partial is prepared.
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x114f)
                {
                    if (before.r[1] >= voiceSamplePlans.size() || !nativePartialSample)
                        throw std::runtime_error("Invalid prepared voice binding");
                    voiceSamplePlans[before.r[1]] = nativePartialSample;
                    if (!nativeEnvelope) throw std::runtime_error("Missing prepared envelope binding");
                    voiceEnvelopePlans[before.r[1]] = nativeEnvelope;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && (before.pc == 0x11ea || before.pc == 0x11fe))
                    throw std::runtime_error("Initial-key probe does not yet support this note entry path");
                if (sampleMode && nativeInitialKey && !before.sleep && before.cp == 0 && before.pc == 0x1219)
                {
                    if (uint8_t(before.r[0]) != *nativeInitialKey
                        || MCU_Read(emu.GetMCU(),0xa1b3) != *nativeInitialKey
                        || MCU_Read(emu.GetMCU(),0xa1b4) != nativeSourceKey)
                    {
                        std::fprintf(stderr,"Initial key mismatch native=%u/%u firmware=%u/%u/%u program=%u coarse=%d\n",
                            *nativeInitialKey,nativeSourceKey,uint8_t(before.r[0]),MCU_Read(emu.GetMCU(),0xa1b3),
                            MCU_Read(emu.GetMCU(),0xa1b4),controls.channel(0).program,controls.channel(0).coarseTuning);
                        std::fprintf(stderr,"part=%u firmware coarse=%u entry r3=%u\n",initialPart,
                            MCU_Read(emu.GetMCU(),0xab46+initialPart),before.r[3]);
                        throw std::runtime_error("Native initial note key differs from firmware");
                    }
                    ++initialKeyChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x132b)
                {
                    auto& cpu = emu.GetMCU();
                    std::array<uint8_t,12> scale;
                    const uint16_t patchBase = uint16_t((MCU_Read(cpu,0xa1c2) << 8) | MCU_Read(cpu,0xa1c3));
                    const auto partialOffset = uint16_t(before.r[5]-patchBase);
                    if (partialOffset != 32 && partialOffset != 124) throw std::runtime_error("Unexpected partial pointer");
                    if (patchBase % SC55Patch::SIZE != 0) throw std::runtime_error("Unaligned live patch");
                    const auto* patchData = soundData.patch(patchBase / SC55Patch::SIZE);
                    if (!patchData) throw std::runtime_error("Missing live patch data");
                    const auto& partial = patchData->partial[partialOffset == 32 ? 0 : 1];
                    const auto partialIndex = partialOffset == 32 ? 0 : 1;
                    if (!completedPatchVelocity || !completedPatchVelocity->partials[partialIndex])
                        throw std::runtime_error("Missing native envelope amplitude");
                    nativeEnvelope = sc55::PreparePartialEnvelope(partial,completedPatchVelocity->partials[partialIndex]->amplitude);
                    if (!nativeEnvelope) throw std::runtime_error("Unsupported envelope sensitivity");
                    const uint16_t part = uint16_t((MCU_Read(cpu,0xa1c0) << 8) | MCU_Read(cpu,0xa1c1));
                    for (unsigned i = 0; i < scale.size(); ++i)
                        scale[i] = MCU_Read(cpu,uint16_t(part+26+i));
                    if (!nativeInitialKey) throw std::runtime_error("Missing native initial key");
                    const uint16_t map = uint16_t((MCU_Read(cpu,0xa1c6) << 8) | MCU_Read(cpu,0xa1c7));
                    nativePartialSample = sc55::PreparePartialSample(partial,sampleBank,*nativeInitialKey,
                        nativeSourceKey,MCU_Read(cpu,0xa1b2),scale,
                        MCU_Read(cpu,partialOffset == 32 ? 0xa1cc : 0xa1cd),
                        MCU_Read(cpu,0xa3d1),MCU_Read(cpu,uint16_t(map+0x180)));
                    if (!nativePartialSample) throw std::runtime_error("Unsupported live partial sample inputs");
                    const auto predicted = nativePartialSample->pitch;
                    if (expectedPitch && (expectedPitch->key != predicted.key || expectedPitch->fraction != predicted.fraction))
                        throw std::runtime_error("Unfinished pitch calculation changed on re-entry");
                    expectedPitch = predicted;
                    expectedResolution = nativePartialSample->key;
                    expectedOriginalNote = expectedResolution->storedOriginalNote.value_or(MCU_Read(cpu,0xa1b2));
                    expectedAdjustedKey = expectedResolution->storedAdjustedKey.value_or(MCU_Read(cpu,0xa1b3));
                }
                if (sampleMode && expectedPitch && !before.sleep && before.cp == 0 && before.pc == 0x1334)
                {
                    auto& cpu = emu.GetMCU();
                    const uint16_t fraction = uint16_t((MCU_Read(cpu,0xa1ca) << 8) | MCU_Read(cpu,0xa1cb));
                    if (uint8_t(before.r[0]) != expectedPitch->key || fraction != expectedPitch->fraction)
                        throw std::runtime_error("Composed native partial pitch differs from firmware");
                    expectedPitch.reset();
                    ++pitchChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x14f0)
                {
                    if (!expectedResolution || expectedResolution->lookupKey != uint8_t(before.r[0])
                        || expectedOriginalNote != MCU_Read(emu.GetMCU(),0xa1b2)
                        || expectedAdjustedKey != MCU_Read(emu.GetMCU(),0xa1b3))
                        throw std::runtime_error("Composed native key resolution differs from firmware");
                    if (!nativePartialSample) throw std::runtime_error("Missing partial sample plan");
                    const std::optional<uint16_t> selected = nativePartialSample->sampleId;
                    // MCU_Step can take an interrupt before executing this PC,
                    // then return to the same entry. Re-observing it is legal.
                    if (expectedSample && expectedSample != selected)
                        throw std::runtime_error("Unfinished sample selection changed on re-entry");
                    expectedSample = selected;
                    if (!expectedSample) throw std::runtime_error("No native sample zone matched");
                    expectedDescriptor = sc55::V121SampleDescriptorOffset(*selected);
                    nativeSampleId = *selected;
                }
                if (sampleMode && expectedSample && !before.sleep && before.cp == 0 && before.pc == 0x1523)
                {
                    if (before.r[4] != *expectedSample) throw std::runtime_error("Native sample zone disagrees with firmware");
                    expectedSample.reset();
                    expectedResolution.reset();
                    ++sampleSelectionChecks;
                }
                if (sampleMode && expectedDescriptor && !before.sleep && before.cp == 0 && before.pc == 0x1557)
                {
                    if (((uint32_t(before.ep) << 16) | before.r[5]) != *expectedDescriptor)
                        throw std::runtime_error("Native descriptor lookup disagrees with firmware");
                    const auto* owned = sampleBank.sample(nativeSampleId);
                    if (!owned) throw std::runtime_error("Missing owned sample descriptor");
                    for (unsigned i = 0; i < owned->data.size(); ++i)
                        if (owned->data[i] != MCU_Read(emu.GetMCU(),
                            (uint32_t(before.ep) << 16) | uint16_t(before.r[5]+i)))
                            throw std::runtime_error("Owned sample descriptor differs from firmware");
                    expectedDescriptor.reset();
                    ++descriptorChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x2bb4)
                {
                    const auto voice = uint16_t((MCU_Read(emu.GetMCU(),uint16_t(before.r[0]-2)) << 8)
                        | MCU_Read(emu.GetMCU(),uint16_t(before.r[0]-1)));
                    if (voice >= voiceSamplePlans.size()) throw std::runtime_error("Invalid startup voice");
                    const auto& plan = voiceSamplePlans[voice];
                    if (!plan || !plan->normalStart
                        || sc55::V121SampleDescriptorOffset(plan->sampleId)
                            != ((uint32_t(before.ep) << 16) | before.r[5]))
                        throw std::runtime_error("Voice startup does not match the native selected sample");
                    const bool unoffset = (MCU_Read(emu.GetMCU(), uint16_t(before.r[0] - 59)) & 0x80) != 0;
                    voiceUnoffset[voice] = unoffset;
                    voiceHistoryNibble[voice] = uint8_t(before.r[3] >> 8) & 15;
                    const auto predicted = *(unoffset ? plan->unoffsetStart : plan->normalStart);
                    if (expectedAddresses && (addressVoice != before.r[0]
                        || expectedAddresses->start != predicted.start || expectedAddresses->loop != predicted.loop
                        || expectedAddresses->end != predicted.end))
                        throw std::runtime_error("Unfinished sample address changed on re-entry");
                    addressVoice = before.r[0];
                    expectedAddresses = predicted;
                }
                if (sampleMode && expectedAddresses && !before.sleep && before.cp == 0 && before.pc == 0x2bef)
                {
                    const auto byte = [&](int displacement) { return MCU_Read(emu.GetMCU(), uint16_t(addressVoice + displacement)); };
                    const auto address = [&](int hi, int lo) { return (uint32_t(byte(hi)) << 16) | (uint32_t(byte(lo)) << 8) | byte(lo+1); };
                    if (address(-20,-16) != expectedAddresses->start || address(-18,-12) != expectedAddresses->loop
                        || address(-19,-14) != expectedAddresses->end)
                        throw std::runtime_error("Native sample-address arithmetic mismatch");
                    expectedAddresses.reset();
                    ++addressChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x2c13)
                {
                    const auto byte = [&](int d) { return MCU_Read(emu.GetMCU(),uint16_t(before.r[0]+d)); };
                    const auto word = [&](int d) { return uint16_t((byte(d) << 8) | byte(d+1)); };
                    const auto voice = word(-2);
                    if (voice >= voiceSamplePlans.size() || !voiceSamplePlans[voice]
                        || !voiceSamplePlans[voice]->normalStart)
                        throw std::runtime_error("Missing native sample control plan");
                    const auto& plan = *voiceSamplePlans[voice];
                    const auto* descriptor = sampleBank.sample(plan.sampleId);
                    if (!descriptor) throw std::runtime_error("Missing sample control descriptor");
                    const auto predicted = sc55::DecodeSampleControl(plan.normalStart->loop,descriptor->data[10],uint8_t(voice),voiceHistoryNibble[voice]);
                    if (word(-10) != predicted.mode || byte(-17) != predicted.loopFlag)
                    {
                        std::fprintf(stderr,"Sample control mismatch voice=%u sample=%u loop=%06x flags=%02x native=%04x/%02x firmware=%04x/%02x\n",
                            voice,plan.sampleId,plan.normalStart->loop,descriptor->data[10],predicted.mode,predicted.loopFlag,word(-10),byte(-17));
                        throw std::runtime_error("Native sample control differs at voice initialization");
                    }
                    ++sampleControlChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x2f52)
                {
                    const auto byte = [&](int d) { return MCU_Read(emu.GetMCU(),uint16_t(before.r[0]+d)); };
                    const auto word = [&](int d) { return uint16_t((byte(d)<<8)|byte(d+1)); };
                    const auto voice = word(-2);
                    if (voice >= voiceEnvelopePlans.size() || !voiceEnvelopePlans[voice])
                        throw std::runtime_error("Missing envelope initialization plan");
                    const auto& plan = *voiceEnvelopePlans[voice];
                    if (word(-30) != plan.velocityScale1 || word(-28) != plan.velocityScale2)
                        throw std::runtime_error("Native live envelope scales differ");
                    for (unsigned i = 0; i < 5; ++i)
                        if (byte(0x4f+i) != plan.stages[i].value || byte(int(i)-8) != plan.stages[i].flag)
                            throw std::runtime_error("Native live envelope stage differs");
                    ++envelopeStageChecks;
                }
                if (sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x5784)
                {
                    if (setupPosition != expectedSetup.size()) throw std::runtime_error("Incomplete sample setup");
                    auto& mcu = emu.GetMCU();
                    const auto byte = [&](int displacement) { return MCU_Read(mcu, uint16_t(mcu.r[0] + displacement)); };
                    const auto word = [&](int displacement) { return uint16_t((byte(displacement) << 8) | byte(displacement+1)); };
                    const auto voice = word(-2);
                    if (voice >= voiceSamplePlans.size() || !voiceSamplePlans[voice]
                        || !voiceSamplePlans[voice]->normalStart)
                        throw std::runtime_error("Missing native sample-address write plan");
                    const auto& plan = *voiceSamplePlans[voice];
                    const auto addresses = *(voiceUnoffset[voice] ? plan.unoffsetStart : plan.normalStart);
                    const auto* descriptor = sampleBank.sample(plan.sampleId);
                    if (!descriptor) throw std::runtime_error("Missing sample control descriptor");
                    const auto control = sc55::DecodeSampleControl(addresses.loop,descriptor->data[10],uint8_t(voice),voiceHistoryNibble[voice]);
                    const sc55::SampleAddressSetup setup {control.mode,addresses.start,addresses.loop,addresses.end};
                    unsigned index = 0;
                    sc55::WriteSampleAddressSetup(setup, [&](uint8_t reg,uint8_t value) {
                        if (index >= expectedSetup.size()) throw std::runtime_error("Oversized native setup");
                        expectedSetup[index++] = {reg,value};
                    });
                    if (index != expectedSetup.size()) throw std::runtime_error("Short native setup");
                    setupPosition = 0;
                }
                if (programUnderTest >= 0 && !before.sleep && before.cp == 0 && before.pc == 0x2d01)
                {
                    const uint32_t offset = ((uint32_t(before.ep) << 16) | before.r[5]) & before.rom2_mask;
                    if (offset < patches.base() || (offset - patches.base()) % SC55Patch::SIZE != 0)
                        throw std::runtime_error("Program selected a non-patch-aligned address");
                    const int index = (offset - patches.base()) / SC55Patch::SIZE;
                    if (index >= patches.size() || (selectedPatch >= 0 && selectedPatch != index))
                        throw std::runtime_error("Program selected inconsistent patch records");
                    selectedPatch = index;
                }
                if (patchMode && !before.sleep && before.cp == 0
                    && (before.pc == 0x2d01 || before.pc == 0x2eeb))
                {
                    const uint64_t key = (uint64_t(before.pc) << 32) | (uint64_t(before.dp) << 16) | before.r[5];
                    if (patchProbes.insert(key).second)
                    {
                        std::fprintf(stderr, "patch-probe pc=%04x r0=%04x r5=%04x dp=%02x ep=%02x tp=%02x\n",
                                     before.pc, before.r[0], before.r[5], before.dp, before.ep, before.tp);
                        if (before.pc == 0x2eeb)
                        {
                            const uint32_t offset = ((uint32_t(before.ep) << 16) | before.r[5]) & before.rom2_mask;
                            if (offset < patches.base()) throw std::runtime_error("Partial precedes patch table");
                            const auto relative = offset - patches.base();
                            const auto index = relative / SC55Patch::SIZE;
                            const auto field = relative % SC55Patch::SIZE;
                            if (index >= static_cast<unsigned>(patches.size())
                                || (field != SC55Patch::PARTIAL_OFFSET && field != SC55Patch::PARTIAL_OFFSET + SC55Partial::SIZE))
                                throw std::runtime_error("Firmware partial pointer disagrees with native layout");
                            const auto& patch = patches[index];
                            std::fprintf(stderr, "native-layout patch=%u name=%s partial=%u offset=%06x matched\n",
                                         index, patch.name.c_str(), (field - SC55Patch::PARTIAL_OFFSET) / SC55Partial::SIZE, offset);
                        }
                    }
                }
                const bool storesControlTime = sampleMode && !before.sleep && before.cp == 0 && before.pc == 0x5af5;
                emu.Step();
                auto& mcu = emu.GetMCU();
                // Step handles interrupts BEFORE executing the instruction. An
                // entry snapshot is not committed unless5c20's clr.w ran.
                if (enteredControllerProbe && (mcu.cp != 0 || mcu.pc != 0x5c22))
                { expectedControllers.reset(); ++deferredControllerEntries; }
                if (storesControlTime && mcu.cp == 0 && mcu.pc == 0x5af9) controlClock.observe(mcu);
                if (!resetSent && (((mcu.dev_register[DEV_SCR] & 0x10) != 0 && mcu.sleep != 0)
                                  || mcu.cycles > 24000000))
                {
                    emu.PostSystemReset(EMU_SystemReset::GS_RESET);
                    resetSent = true;
                }
                if (mcu.cycles > cycleLimit) throw std::runtime_error("Audio clock stopped advancing");
            }
        };
        run(5.0);
        phase = "idle"; run(1.0);
        if (outputControlMode)
        {
            for (unsigned channel = 0; channel < 16; ++channel)
            {
                const uint8_t messages[] {uint8_t(0xb0|channel),7,80,11,90,10,22,
                    91,uint8_t(17+channel),93,uint8_t(91-channel),uint8_t(0x90|channel),60,100};
                send(messages); run(0.075);
                const uint8_t off[] {uint8_t(0x80|channel),60,0}; send(off); run(0.05);
            }
            if (outputControlParts != 0xffff || !outputControlChecks)
                throw std::runtime_error("Missing live MIDI output-control part coverage");
            std::printf("Native MIDI-to-voice output controls: %u checks, all16 parts matched\n",outputControlChecks);
        }
        if (holdMode)
        {
            sc55::VoiceAllocator native;
            visitAllocatorBytes(native,[&](unsigned address,uint8_t& value) { value = MCU_Read(emu.GetMCU(),address); });
            sc55::MidiDecoder holdDecoder;
            unsigned checks = 0;
            for (unsigned channel = 0; channel < 16; ++channel)
            {
                const unsigned part = channel == 9 ? 0 : channel < 9 ? channel+1 : channel;
                for (const uint8_t value : {0,1,63,64,65,127,127,0})
                {
                    const uint8_t cc[] {uint8_t(0xb0|channel),64,value};
                    std::array<uint8_t,16> retained;
                    for (unsigned i = 0; i < 16; ++i) retained[i] = MCU_Read(emu.GetMCU(),0xa090+16*part+i);
                    for (const auto byte : cc)
                        holdDecoder.push(std::span(&byte,1),[&](const auto& event) {
                            if (!sc55::ApplyRoutedHoldController(event,part,true,retained,native))
                                throw std::runtime_error("Native routed hold rejected MIDI");
                        });
                    send(cc); run(0.025);
                    for (unsigned p = 0; p < 16; ++p)
                        if (native.partFlags[p] != MCU_Read(emu.GetMCU(),0xa230+p))
                            throw std::runtime_error("MIDI hold routing/threshold differs from H8");
                    ++checks;
                    std::printf("hold-route channel=%u part=%u value=%u flag=%u\n",channel,part,value,
                        MCU_Read(emu.GetMCU(),0xa230+part));
                }
            }
            std::printf("Native MIDI hold routing: %u messages, all16 part flags matched\n",checks);
            unsigned sostenutoChecks = 0;
            for (unsigned channel = 0; channel < 16; ++channel)
            {
                const unsigned part = channel == 9 ? 0 : channel < 9 ? channel+1 : channel;
                const uint8_t on[] {uint8_t(0x90|channel),60,100}; send(on); run(0.05);
                for (const uint8_t value : {63,64,127,0})
                {
                    if (value == 127)
                    { const uint8_t more[] {uint8_t(0x90|channel),61,100}; send(more); run(0.05); }
                    visitAllocatorBytes(native,[&](unsigned address,uint8_t& v) { v = MCU_Read(emu.GetMCU(),address); });
                    std::array<uint8_t,16> retained;
                    for (unsigned i = 0; i < 16; ++i) retained[i] = MCU_Read(emu.GetMCU(),0xa090+16*part+i);
                    const auto bitsBefore = unsigned(MCU_Read(emu.GetMCU(),0xab00))*256 + MCU_Read(emu.GetMCU(),0xab01);
                    bool enabled = (bitsBefore&(1u<<part)) != 0;
                    const uint8_t cc[] {uint8_t(0xb0|channel),66,value};
                    for (auto byte : cc) holdDecoder.push(std::span(&byte,1),[&](const auto& event) {
                        if (!sc55::ApplyRoutedSostenutoController(event,part,true,enabled,retained,native))
                            throw std::runtime_error("Native routed sostenuto rejected MIDI");
                    });
                    send(cc); run(0.025);
                    const auto expectedBits = (bitsBefore&~(1u<<part))|(unsigned(enabled)<<part);
                    const auto actualBits = unsigned(MCU_Read(emu.GetMCU(),0xab00))*256 + MCU_Read(emu.GetMCU(),0xab01);
                    if (expectedBits != actualBits) throw std::runtime_error("Sostenuto part bits differ from H8");
                    for (unsigned i = 0; i < 16; ++i)
                        if (retained[i] != MCU_Read(emu.GetMCU(),0xa090+16*part+i))
                            throw std::runtime_error("MIDI sostenuto retained keys differ from H8");
                    ++sostenutoChecks;
                }
                const uint8_t off[] {uint8_t(0x80|channel),60,0,61,0}; send(off); run(0.05);
            }
            std::printf("Native MIDI sostenuto: %u messages, retained rows and all16 part bits matched\n",sostenutoChecks);
            // Deliberately remap the research emulator's part configuration.
            // Restore it afterwards; no production state or ROM is changed.
            std::array<sc55::PartMidiReceive,16> routing, savedRouting;
            sc55::PartNoteState routedNotes;
            visitAllocatorBytes(routedNotes.allocator,[&](unsigned a,uint8_t& v) { v = MCU_Read(emu.GetMCU(),a); });
            for (unsigned part = 0; part < 16; ++part)
            {
                const auto base = 0x8048+part*0x70;
                savedRouting[part] = {MCU_Read(emu.GetMCU(),base+4),uint16_t(
                    unsigned(MCU_Read(emu.GetMCU(),base+2))*256+MCU_Read(emu.GetMCU(),base+3))};
                routing[part] = {uint8_t(part == 15 ? 255 : part < 8 ? 7 : 2),
                    uint16_t(((part&1) ? 0x0800 : 0)|((part&2) ? 0x0020 : 0)|((part&4) ? 0x0080 : 0))};
                MCU_Write(emu.GetMCU(),base+4,routing[part].channel);
                MCU_Write(emu.GetMCU(),base+2,uint8_t(routing[part].flags>>8));
                MCU_Write(emu.GetMCU(),base+3,uint8_t(routing[part].flags));
            }
            unsigned routingChecks = 0;
            for (unsigned channel : {7u,2u,9u})
                for (uint8_t controller : {64,66})
                    for (uint8_t value : {63,64,127,0})
                    {
                        const uint8_t cc[] {uint8_t(0xb0|channel),controller,value};
                        for (auto byte : cc) holdDecoder.push(std::span(&byte,1),[&](const auto& event) {
                            if (!routedNotes.receivePedal(event,routing))
                                throw std::runtime_error("Native pedal fan-out rejected MIDI");
                        });
                        send(cc); run(0.025);
                        const auto bits = unsigned(MCU_Read(emu.GetMCU(),0xab00))*256+MCU_Read(emu.GetMCU(),0xab01);
                        for (unsigned part = 0; part < 16; ++part)
                        {
                            if ((routedNotes.allocator.partFlags[part]&1) != (MCU_Read(emu.GetMCU(),0xa230+part)&1)
                                || routedNotes.part(part)->sostenutoEnabled != bool(bits&(1u<<part)))
                                throw std::runtime_error("Native remapped pedal receive gates differ from H8");
                        }
                        ++routingChecks;
                    }
            for (unsigned part = 0; part < 16; ++part)
            {
                const auto base = 0x8048+part*0x70;
                MCU_Write(emu.GetMCU(),base+4,savedRouting[part].channel);
                MCU_Write(emu.GetMCU(),base+2,uint8_t(savedRouting[part].flags>>8));
                MCU_Write(emu.GetMCU(),base+3,uint8_t(savedRouting[part].flags));
            }
            std::printf("Native pedal fan-out: %u messages, remapped/disabled receive gates matched H8\n",routingChecks);
        }
        if (programMode)
        {
            phase = "program_probe";
            for (int program = 0; program < 128; ++program)
            {
                const uint8_t silence[] {0xb0,120,0}; send(silence); run(0.1);
                const uint8_t select[] {0xc0,uint8_t(program)}; send(select); run(0.1);
                selectedPatch = -1;
                programUnderTest = program;
                const uint8_t note[] {0x90,60,100}; send(note); run(0.1);
                if (selectedPatch < 0) throw std::runtime_error("Program did not reach patch expansion");
                const auto native = sc55::ResolveV121MelodicPreset(0, controls.channel(0).program);
                if (!native || *native != selectedPatch)
                    throw std::runtime_error("Native program resolution disagrees with firmware");
                std::printf("program-map %d %d %s\n", program, selectedPatch, patches[selectedPatch].name.c_str());
                programUnderTest = -1;
                const uint8_t release[] {0x80,60,0}; send(release); run(0.1);
            }
            std::puts("Native capital-tone resolution: all 128 programs matched firmware");
        }
        if (controllerMode)
        {
            unsigned checks = 0;
            const auto check = [&]
            {
                for (unsigned channel = 0; channel < 16; ++channel)
                {
                    // Default GS part routing; reassignment via SysEx remains separate.
                    const unsigned part = channel == 9 ? 0 : channel < 9 ? channel + 1 : channel;
                    const uint32_t base = 0x8048 + part * 0x70;
                    const auto& native = controls.channel(channel);
                    const unsigned softMask = (MCU_Read(emu.GetMCU(),0xab02) << 8) | MCU_Read(emu.GetMCU(),0xab03);
                    if (bool(softMask & (1u << part)) != native.softPedal)
                        throw std::runtime_error("Native soft-pedal state differs from firmware");
                    ++checks;
                    const std::pair<uint32_t, uint8_t> values[] {
                        {base + 8, native.volume}, {base + 9, native.pan},
                        {base + 14, native.chorus}, {base + 15, native.reverb},
                        {0xab36 + part, native.expression}, {0xab46 + part, uint8_t(native.coarseTuning)}
                    };
                    for (auto [address, expected] : values)
                    {
                        const auto actual = MCU_Read(emu.GetMCU(), address);
                        if (actual != expected)
                        {
                            std::fprintf(stderr, "controller mismatch ch=%u address=%04x firmware=%u native=%u\n",
                                         channel, address, actual, expected);
                            throw std::runtime_error("Native channel state mismatch");
                        }
                        ++checks;
                    }
                }
            };
            check();
            phase = "controller_probe";
            if (!rpnMode) for (unsigned channel = 0; channel < 16; ++channel)
                for (uint8_t cc : {7, 10, 11, 67, 91, 93})
                    for (unsigned value = 0; value < 128; ++value)
                    {
                        const uint8_t message[] {uint8_t(0xb0 | channel), cc, uint8_t(value)};
                        send(message); run(0.025); check();
                    }
            if (rpnMode) for (unsigned channel = 0; channel < 16; ++channel)
            {
                const uint8_t status = uint8_t(0xb0 | channel);
                const uint8_t select[] {status,101,0,100,2};
                send(select); run(0.025); check();
                for (unsigned value = 0; value < 128; ++value)
                {
                    const uint8_t message[] {status,6,uint8_t(value)};
                    send(message); run(0.025); check();
                }
                const uint8_t deselect[] {status,101,127,100,127,6,64};
                send(deselect); run(0.025); check();
                const uint8_t nrpn[] {status,101,0,100,2,99,0,98,2,6,64};
                send(nrpn); run(0.025); check();
            }
            std::printf("Native channel state: %u firmware comparisons passed\n", checks);
        }
        if (tunedMode)
        {
            phase = "tuned_note_probe";
            const uint8_t selectRpn[] {0xb0,101,0,100,2}; send(selectRpn); run(0.025);
            unsigned notes = 0;
            unsigned unobservedNotes = 0;
            for (unsigned program = 0; program < 128; ++program)
            {
                const uint8_t select[] {0xc0,uint8_t(program)}; send(select); run(0.025);
                for (uint8_t coarse : {0,40,64,88,127})
                {
                    if (softMode)
                    {
                        const uint8_t soft[] {0xb0,67,uint8_t(coarse >= 88 ? 127 : 0)};
                        send(soft); run(0.025);
                    }
                    const uint8_t tune[] {0xb0,6,coarse}; send(tune); run(0.025);
                    if (uint8_t(controls.channel(0).coarseTuning) != MCU_Read(emu.GetMCU(),0xab47))
                    {
                        std::fprintf(stderr,"Coarse update mismatch program=%u data=%u fw=%u rpn=%02x%02x nrpnMask=%02x%02x\n",
                            program,coarse,MCU_Read(emu.GetMCU(),0xab47),MCU_Read(emu.GetMCU(),0xabb8),
                            MCU_Read(emu.GetMCU(),0xabb9),MCU_Read(emu.GetMCU(),0xab04),MCU_Read(emu.GetMCU(),0xab05));
                        throw std::runtime_error("Coarse update not reflected by reference firmware");
                    }
                    for (uint8_t key : {0,24,60,103,127})
                    {
                        const uint8_t silence[] {0xb0,120,0}; send(silence); run(0.025);
                        const unsigned beforeKeyChecks = initialKeyChecks;
                        const unsigned beforeAbsent = absentHighNotes;
                        const uint8_t on[] {0x90,key,100}; send(on); run(0.05);
                        if (initialKeyChecks == beforeKeyChecks)
                        {
                            if (absentHighNotes == beforeAbsent)
                            {
                                std::fprintf(stderr,"Unprocessed tuned note program=%u coarse=%u key=%u\n",program,coarse,key);
                                ++unobservedNotes;
                            }
                        }
                        const uint8_t off[] {0x80,key,0}; send(off); run(0.025);
                        ++notes;
                    }
                }
            }
            const uint8_t restore[] {0xb0,6,64,101,127,100,127}; send(restore); run(0.025);
            if (softMode) { const uint8_t softOff[] {0xb0,67,0}; send(softOff); run(0.025); }
            std::printf("Tuned-note sweep: %u sent, %u absent high-note mappings checked, %u UNVERIFIED entry paths\n",
                        notes,absentHighNotes,unobservedNotes);
            if (unobservedNotes) throw std::runtime_error("Tuned-note sweep contains unverified entry paths");
        }
        if (monoMode)
        {
            phase = "mono_probe";
            const uint8_t setup[] {0xb0,120,0,126,0,0xc0,0}; send(setup); run(0.1);
            for (const auto& note : {std::array<uint8_t,3>{0x90,60,40}, {0x90,64,80}, {0x90,67,110}})
            { send(note); run(0.1); }
            for (uint8_t key : {64,67,60})
            { const uint8_t off[] {0x80,key,0}; send(off); run(0.1); }
            for (auto count : monoBranches) if (!count) throw std::runtime_error("Missing live mono branch coverage");
            if (monoDecision || monoPreparing || monoPreparationChecks != 2)
                throw std::runtime_error("Unfinished live mono preparation");
            std::printf("Live native mono decisions: %u unchanged, %u release, %u replace\n",
                monoBranches[0],monoBranches[1],monoBranches[2]);
            const uint8_t restore[] {0xb0,127,0}; send(restore); run(0.1);
        }
        const uint8_t program[] {0xc0, 16};
        send(program);
        phase = "program"; run(0.1);
        const uint8_t on[] {0x90, 60, 100};
        send(on);
        phase = "note_on";
        const auto before = nonzero;
        run(1.0);
        if (nonzero == before) throw std::runtime_error("Note-on produced no nonzero samples");
        const uint8_t expression[] {0xb0, 11, 64};
        send(expression);
        phase = "expression"; run(0.5);
        const uint8_t off[] {0x80, 60, 0};
        send(off);
        phase = "note_off"; run(2.0);
        if (monoMode || sampleMode)
        {
            if (!dispatchChecks || liveDispatch) throw std::runtime_error("Incomplete live partial dispatch comparison");
            if (sampleMode && !dualDispatchChecks) throw std::runtime_error("Missing live dual-partial coverage");
            std::printf("Live native partial dispatch: %u matched, %u dual preparation, %u shared slot\n",
                dispatchChecks,dualDispatchChecks,sharedDispatchChecks);
        }
        if (sampleMode)
        {
            if (!pitchChecks || expectedPitch) throw std::runtime_error("Incomplete composed pitch probe");
            if (!liveControllerChecks || expectedControllers) throw std::runtime_error("Incomplete live controller preparation");
            std::printf("Native live controller preparation: %u complete eleven-field matches\n",liveControllerChecks);
            std::printf("Controller entry interrupted before execution: %u snapshots discarded\n",deferredControllerEntries);
            controlClock.report();
            if (!candidateChecks || expectedCandidates) throw std::runtime_error("Incomplete partial-candidate probe");
            std::printf("Native partial candidates: %u matched\n",candidateChecks);
            if (!velocityOperationChecks || velocityPending) throw std::runtime_error("Incomplete velocity operation probe");
            std::printf("Native full velocity operation: %u matched\n",velocityOperationChecks);
            if (!initialKeyChecks) throw std::runtime_error("Initial key probe did not execute");
            std::printf("Native initial note keys: %u matched\n", initialKeyChecks);
            std::printf("Native composed partial pitch: %u matched\n", pitchChecks);
            if (!sampleSelectionChecks || expectedSample) throw std::runtime_error("Incomplete sample selection probe");
            std::printf("Native sample-zone selections: %u matched\n", sampleSelectionChecks);
            if (!descriptorChecks || expectedDescriptor) throw std::runtime_error("Incomplete descriptor lookup probe");
            std::printf("Native descriptor lookups: %u matched\n", descriptorChecks);
            if (!addressChecks || expectedAddresses) throw std::runtime_error("Incomplete sample-address arithmetic probe");
            std::printf("Native sample-address calculations: %u matched\n", addressChecks);
            if (!sampleControlChecks) throw std::runtime_error("No sample control comparisons");
            std::printf("Native sample-control calculations: %u matched\n",sampleControlChecks);
            if (!envelopeStageChecks) throw std::runtime_error("No envelope initialization comparisons");
            std::printf("Native envelope initialization: %u matched\n",envelopeStageChecks);
            if (!envelopeUpdateChecks || expectedEnvelopeUpdate) throw std::runtime_error("Incomplete envelope update probe");
            std::printf("Native composed envelope updates: %u matched\n",envelopeUpdateChecks);
            std::printf("Native live envelope durations: %u matched\n",liveDurationChecks);
            if (!stageChecks || expectedStage) throw std::runtime_error("Incomplete envelope stage probe");
            std::printf("Native live envelope transitions: %u matched\n",stageChecks);
            if (!liveReleaseChecks || expectedRelease) throw std::runtime_error("Incomplete envelope release probe");
            std::printf("Native live envelope releases: %u matched\n",liveReleaseChecks);
            if (!liveStartChecks || expectedEnvelopeStart) throw std::runtime_error("Incomplete native envelope initialization");
            std::printf("Native full envelope starts: %u matched\n",liveStartChecks);
            if (!liveActivationChecks || activatingEnvelope) throw std::runtime_error("Incomplete native envelope activation");
            std::printf("Native live envelope activations: %u matched\n",liveActivationChecks);
            if (!persistentUpdateChecks || updatingNativeEnvelope) throw std::runtime_error("Incomplete persistent envelope comparisons");
            std::printf("Native persistent live updates: %u matched\n",persistentUpdateChecks);
            if (!syncWriteChecks || expectedEnvelopeSync) throw std::runtime_error("Incomplete native PCM sync writes");
            std::printf("Native envelope PCM sync writes: %llu matched\n",static_cast<unsigned long long>(syncWriteChecks));
            if (!terminationWriteChecks || terminationByte != 3) throw std::runtime_error("Incomplete termination transaction");
            std::printf("Native envelope PCM termination writes: %llu matched\n",static_cast<unsigned long long>(terminationWriteChecks));
            if (!liveDetachChecks || expectedDetachedLinks) throw std::runtime_error("Incomplete native detach comparison");
            std::printf("Native live voice detaches: %u matched\n",liveDetachChecks);
            if (!liveVoiceReturnChecks || expectedVoiceReturn) throw std::runtime_error("Incomplete native voice return comparison");
            std::printf("Native live voice returns: %u matched\n",liveVoiceReturnChecks);
            if (!liveVoiceTakeChecks || !liveVoiceAttachChecks || expectedVoiceTake || expectedVoiceAttach)
                throw std::runtime_error("Incomplete native voice allocation comparison");
            std::printf("Native live free voice selections: %u matched; attaches: %u matched\n",liveVoiceTakeChecks,liveVoiceAttachChecks);
            if (!liveGroupCreationChecks || expectedGroupCreation) throw std::runtime_error("Incomplete native group creation comparison");
            std::printf("Native live group creations: %u matched\n",liveGroupCreationChecks);
            if (!liveAllocatorInitializationChecks || expectedAllocatorInitialization)
                throw std::runtime_error("Incomplete native allocator initialization comparison");
            std::printf("Native live allocator initializations: %u matched\n",liveAllocatorInitializationChecks);
            if (!liveNoteReleaseChecks || expectedNoteRelease)
                throw std::runtime_error("Incomplete live note release comparison");
            std::printf("Native live note-release selections: %u matched\n",liveNoteReleaseChecks);
            if (setupChecks == 0 || setupPosition != expectedSetup.size())
                throw std::runtime_error("Sample-address verification did not complete");
            std::printf("Native sample-address writes: %llu matched\n", static_cast<unsigned long long>(setupChecks));
            if (livePitchInputChecks == 0) throw std::runtime_error("No live pitch input coverage");
            std::printf("Native live pitch inputs: %u matched\n",livePitchInputChecks);
        }
        const bool failed = std::ferror(trace) != 0;
        const int closed = std::fclose(trace); trace = nullptr;
        if (failed || closed != 0) throw std::runtime_error("Trace write failed");
        std::printf("romset=%s frames=%llu writes=%llu nonzero=%llu rate=%u\n",
                    roms.picked_name.c_str(), static_cast<unsigned long long>(frames),
                    static_cast<unsigned long long>(writes), static_cast<unsigned long long>(nonzero),
                    PCM_GetOutputFrequency(emu.GetPCM()));
        return 0;
    }
    catch (const std::exception& e)
    {
        if (trace != nullptr) std::fclose(trace);
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
