#pragma once
#include "sc55_sound_data_import.h"

inline void VerifyRhythmPresets(Emulator& emu)
{
    auto& cpu = emu.GetMCU();
    const std::vector<uint8_t> rom1(cpu.rom1,cpu.rom1+32768), rom2(cpu.rom2,cpu.rom2+262144);
    const auto presets = sc55::ImportRhythmPresets(rom1,rom2);
    const auto run = [&](unsigned duration) {
        const auto end = cpu.cycles+duration;
        while (cpu.cycles < end) emu.Step();
    };
    run(120000000);
    sc55::RhythmPresetTable::Record expected = presets.records[presets.programs[0]];
    unsigned mapBase = 0x8748;
    const auto compare = [&]() {
        for (unsigned i = 0; i < expected.size(); ++i)
            if (MCU_Read(cpu,mapBase+i) != expected[i])
                throw std::runtime_error("Rhythm map mismatch at byte "+std::to_string(i));
        const auto decoded = sc55::RhythmPresetTable::decode(expected);
        for (unsigned key = 0; key < 128; ++key)
            if (decoded.tones[key] != MCU_Read16(cpu,mapBase+2*key)
                || decoded.pitches[key] != MCU_Read(cpu,mapBase+0x180+key)
                || decoded.groups[key] != MCU_Read(cpu,mapBase+0x200+key)
                || decoded.flags[key] != MCU_Read(cpu,mapBase+0x400+key))
                throw std::runtime_error("Decoded rhythm map mismatch");
    };
    compare();
    for (unsigned mapIndex = 0; mapIndex < 2; ++mapIndex)
    {
      // Select the other map directly for this focused program-copy probe;
      // this does not test GS mode-change/reset side effects.
      MCU_Write(cpu,0x804d,mapIndex == 0 ? 0xb0 : 0xd0);
      mapBase = 0x8748+mapIndex*0x48c;
      for (unsigned program = 0; program < 128; ++program)
      {
        const uint8_t message[]{0xc9,uint8_t(program)};
        emu.PostMIDI(message); run(1000000);
        const auto resolved = presets.resolve(program);
        if (resolved) expected = presets.records[presets.programs[*resolved]];
        compare();
        if ((MCU_Read16(cpu,0xab06) == 0xffff) != !resolved)
            throw std::runtime_error("Rhythm invalid-program state mismatch");
      }
    }
    std::puts("Rhythm presets PASS: boot and 128 MIDI programs on both maps, complete records and decoded note rows");
    unsigned writes = 0;
    for (unsigned map = 0; map < 2; ++map)
    {
        MCU_Write(cpu,0x804d,map == 0 ? 0xb0 : 0xd0);
        const uint8_t program[]{0xc9,0}; emu.PostMIDI(program); run(1000000);
        mapBase = 0x8748+map*0x48c;
        expected = presets.records[presets.programs[0]];
        const auto send = [&](unsigned field,unsigned key,std::initializer_list<uint8_t> values,
            sc55::RhythmPresetTable::WriteResult expectedResult = sc55::RhythmPresetTable::WriteResult::applied) {
            std::vector<uint8_t> message{0xf0,0x41,0x10,0x42,0x12,0x41,uint8_t(map*16+field),uint8_t(key)};
            unsigned sum = 0x41+map*16+field+key;
            for (auto value : values) { message.push_back(value); sum += value; }
            message.push_back(uint8_t((-sum)&127)); message.push_back(0xf7);
            if (sc55::RhythmPresetTable::write(expected,field,key,{values.begin(),values.size()}) != expectedResult)
                throw std::runtime_error("Unexpected native drum write result");
            emu.PostMIDI(message); run(1000000); compare(); ++writes;
        };
        for (unsigned field = 1; field <= 6; ++field)
        {
            send(field,38,{17}); send(field,60,{0,1,64,127});
            send(field,127,{31,32}); // byte-row continuation, including its boundary
        }
        for (unsigned field : {7u,8u})
        {
            send(field,38,{0}); send(field,38,{127}); send(field,38,{0,127});
        }
        send(0,0,{0,1,31,32,65,66,67,68,69,70,71,127});
        send(0,0,{65,66},sc55::RhythmPresetTable::WriteResult::invalidLength);
        send(0,1,{65},sc55::RhythmPresetTable::WriteResult::unsupported);
    }
    std::printf("Rhythm GS PASS: %u writes, full-record comparisons on both maps\n",writes);
}
