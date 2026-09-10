#pragma once
#include "sc55_sound_data_import.h"
#include "sc55_part_settings.h"

inline void VerifyMelodicPresets(Emulator& emu)
{
    auto& cpu = emu.GetMCU();
    const std::vector<uint8_t> rom1(cpu.rom1,cpu.rom1+32768),rom2(cpu.rom2,cpu.rom2+262144);
    const auto presets = sc55::ImportMelodicPresets(rom1,rom2);
    const auto run = [&](unsigned duration) {
        const auto end = cpu.cycles+duration; while (cpu.cycles<end) emu.Step();
    };
    run(120000000);
    for (unsigned bank = 0; bank < 128; ++bank)
    {
        const auto before = MCU_Read16(cpu,0xab08);
        const uint8_t select[]{0xb0,0,uint8_t(bank)}; emu.PostMIDI(select); run(100000);
        if (MCU_Read16(cpu,0xab08) != before) throw std::runtime_error("Bank latch changed selected tone early");
        for (unsigned program = 0; program < 128; ++program)
        {
            const uint8_t message[]{0xc0,uint8_t(program)}; emu.PostMIDI(message); run(100000);
            const auto expected = presets.resolve(bank,program);
            const auto tone = MCU_Read16(cpu,0xab08);
            if (tone != (expected ? expected->tone : 0xffff)
                || (expected && MCU_Read16(cpu,0xce36) != ((unsigned(expected->bank)<<8)|program)))
            {
                std::printf("Melodic selection mismatch bank=%u program=%u H8=%u native=%u\n",
                    bank,program,tone,expected ? expected->tone : 0xffff);
                throw std::runtime_error("Native melodic preset mismatch");
            }
        }
    }
    std::puts("Melodic presets PASS: 16384 bank/program selections and 128 deferred bank latches matched H8");
    sc55::PartSettings parts;
    unsigned checks = 0;
    const auto gs = [&](std::initializer_list<uint8_t> payload,bool valid) {
        const auto before = MCU_Read16(cpu,0xab08);
        std::vector<uint8_t> message{0xf0,0x41,0x10,0x42,0x12}; unsigned sum = 0;
        for (auto byte : payload) { message.push_back(byte); sum += byte; }
        message.push_back(uint8_t((-sum)&127)); message.push_back(0xf7);
        bool selected = false;
        auto expectedTone = before;
        const auto result = parts.write({payload.begin(),payload.size()},[](unsigned) { return false; },
            [&](unsigned part,uint8_t bank,uint8_t program) {
                if (part != 1) throw std::runtime_error("GS tone addressed wrong part");
                selected = true; parts.parts[part].bank = bank; parts.parts[part].controls.program = program;
                const auto tone = presets.resolve(bank,program); expectedTone = tone ? tone->tone : 0xffff;
                return true;
            });
        if (selected != valid || (result == sc55::PartSettings::WriteResult::applied) != valid)
            throw std::runtime_error("GS tone length contract mismatch");
        emu.PostMIDI(message); run(1000000);
        if (MCU_Read16(cpu,0xab08) != expectedTone
            || (valid && (MCU_Read(cpu,0x80b8) != parts.parts[1].bank
                || MCU_Read(cpu,0x80b9) != parts.parts[1].controls.program
                || MCU_Read(cpu,0xab67) != parts.parts[1].bank)))
            throw std::runtime_error("GS tone selection differs from H8");
        ++checks;
    };
    // GS addressing bypasses ordinary MIDI program/channel receive gates.
    MCU_Write16(cpu,0x80ba,0);
    for (const auto pair : {std::array<uint8_t,2>{0,48},{8,0},{16,0},{127,0},{64,0},{0,80}})
        gs({0x40,0x11,0,pair[0],pair[1]},true);
    gs({0x40,0x11,0,8},false);
    gs({0x40,0x11,0,8,4,127},false);
    std::printf("GS melodic tone PASS: %u pairs/length cases with MIDI receive disabled\n",checks);
}
