#pragma once
#include "sc55_part_settings.h"
#include "sc55_rhythm_presets.h"
#include "sc55_sound_data_import.h"
inline void VerifyNrpn(Emulator& emu)
{
    auto& cpu=emu.GetMCU(); sc55::PartSettings native;
    const auto run=[&](unsigned cycles) { auto end=cpu.cycles+cycles; while(cpu.cycles<end) emu.Step(); };
    run(120000000);
    unsigned count=0;
    const auto send=[&](uint8_t cc,uint8_t value) {
        const uint8_t message[]{0xb9,cc,value};
        sc55::MidiDecoder decoder;
        decoder.push(message,[&](auto event) { native.applyScalar(event); });
        auto& current=native.parts[0].controls;
        if (cc==6 && current.nrpnSelected && (native.routing[0].flags&0x8800)==0x8800)
            current.tone.writeNrpn(current.nrpnMsb,current.nrpnLsb,value);
        emu.PostMIDI(message); run(200000);
        const auto& c=native.parts[0].controls;
        if (c.nrpnSelected!=bool(MCU_Read16(cpu,0xab04)&1)
            || ((unsigned(c.nrpnMsb)<<8)|c.nrpnLsb)!=MCU_Read16(cpu,0xabd6))
            throw std::runtime_error("Native NRPN selector differs from H8");
        ++count;
        for (unsigned i=0;i<8;++i)
            if (current.tone.values[i]!=MCU_Read(cpu,0x8058+i))
                throw std::runtime_error("Native tone NRPN differs from H8");
    };
    // Disabled NRPN receive still changes mode, but not selector bytes.
    send(99,0x1a); send(98,38); send(101,0);
    const uint8_t reset[]{0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7};
    emu.PostMIDI(reset); run(10000000);
    native=sc55::PartSettings{}; for (auto& route:native.routing) route.flags|=0x8000;
    for (uint8_t msb:{uint8_t(0x1a),uint8_t(0x1c),uint8_t(0x1d),uint8_t(0x1e)})
        for (uint8_t key:{uint8_t(0),uint8_t(38),uint8_t(127)}) {
            send(99,msb); send(98,key);
            for (uint8_t value:{uint8_t(0),uint8_t(64),uint8_t(127)}) {
                send(6,value);
                const auto offset=sc55::RhythmPresetTable::nrpnOutputOffset(msb,key);
                if (!offset || MCU_Read(cpu,0x8748+*offset)!=value)
                    throw std::runtime_error("Native drum NRPN output mapping differs from H8");
            }
        }
    send(99,1);
    for (uint8_t number:{uint8_t(8),uint8_t(9),uint8_t(0x20),uint8_t(0x21),
        uint8_t(0x63),uint8_t(0x64),uint8_t(0x66),uint8_t(0x0a)}) {
        send(98,number);
        for (uint8_t value:{uint8_t(0),uint8_t(14),uint8_t(64),uint8_t(80),uint8_t(114),uint8_t(127)})
            send(6,value);
    }
    const auto presets=sc55::ImportRhythmPresets(std::vector<uint8_t>(cpu.rom1,cpu.rom1+32768),
        std::vector<uint8_t>(cpu.rom2,cpu.rom2+262144));
    unsigned pitchChecks=0;
    for (unsigned p=0;p<128;++p) {
        const auto program=uint8_t(p);
        const uint8_t change[]{0xc9,program}; emu.PostMIDI(change); run(400000);
        const auto selected=presets.resolve(program).value_or(program);
        if (MCU_Read16(cpu,0xce34)!=selected)
            throw std::runtime_error("Native drum committed program differs from H8");
        sc55::RhythmPresetTable::Record record{};
        for (unsigned i=0;i<record.size();++i) record[i]=MCU_Read(cpu,0x8748+i);
        for (uint8_t key:{uint8_t(0),uint8_t(38),uint8_t(127)}) {
            send(99,0x18); send(98,key);
            for (uint8_t value:{uint8_t(0),uint8_t(127),uint8_t(76),uint8_t(76),uint8_t(64)}) {
                presets.writeRelativePitch(record,selected,key,value);
                send(6,value);
                if (record[0x180+key]!=MCU_Read(cpu,0x88c8+key))
                {
                    std::printf("Pitch program=%u key=%u value=%u native=%u H8=%u latch=%04x\n",program,key,value,
                        record[0x180+key],MCU_Read(cpu,0x88c8+key),MCU_Read16(cpu,0xce34));
                    throw std::runtime_error("Native NRPN relative pitch differs from H8");
                }
                ++pitchChecks;
            }
        }
    }
    std::printf("Native NRPN PASS: %u messages, all8 tone controls, %u relative pitch comparisons\n",count,pitchChecks);
}
