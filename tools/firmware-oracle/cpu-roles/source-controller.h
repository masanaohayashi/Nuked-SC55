#pragma once
#include "sc55_native_player.h"

inline void VerifySourceController(Emulator& emu,const RomsetInfo& roms)
{
    const auto& raw=roms.rom_data;
    const auto& r1=raw[size_t(RomLocation::ROM1)];
    const auto& r2=raw[size_t(RomLocation::ROM2)];
    sc55::SoundData data;
    if(!data.loadEncoded(sc55::ImportSoundData(r1,r2))) throw std::runtime_error("Invalid sound data");
    auto pcm=std::make_unique<pcm_t>();pcm->is_mk1=true;
    std::copy_n(emu.GetPCM().waverom1,sizeof(pcm->waverom1),pcm->waverom1);
    std::copy_n(emu.GetPCM().waverom2,sizeof(pcm->waverom2),pcm->waverom2);
    std::copy_n(emu.GetPCM().waverom3,sizeof(pcm->waverom3),pcm->waverom3);
    sc55::NativeMelodicPlayer player(data,*pcm,sc55::ImportSystemDefaults(r1,r2),
        sc55::ImportRhythmPresets(r1,r2),sc55::ImportMelodicPresets(r1,r2),sc55::ImportEffectsTables(r1,r2));
    auto& cpu=emu.GetMCU();
    const auto boot=cpu.cycles+120000000;
    while(cpu.cycles<boot) emu.Step();
    player.renderFrames(16384);
    const auto send=[&](const std::vector<uint8_t>& packet) {
        emu.PostMIDI(packet);
        if(player.push(packet)!=packet.size()) throw std::runtime_error("Source-controller queue full");
        const auto end=cpu.cycles+2000000;
        while(cpu.cycles<end) emu.Step();
        player.renderFrames(3200);
        if(player.failed()) throw std::runtime_error("Source-controller native failure");
    };
    const auto select=[&](unsigned cc) {
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7e,uint8_t(cc),uint8_t(-(0x40 + 0x7e + cc)&127),0xf7});
    };
    unsigned checks=0,mismatches=0;
    for(unsigned channel:{0u,9u}) for(unsigned cc=0;cc<128;++cc) for(unsigned value:{0u,48u,127u}) {
        select(84);
        send({uint8_t(0xb0+channel),121,0});
        select(cc);
        send({uint8_t(0xb0+channel),uint8_t(cc),uint8_t(value)});
        const unsigned part=channel==9 ? 0 : 1;
        const auto& settings=player.partSettings().parts[part];
        const unsigned base=0x8048+part*0x70;
        // CC0 changes the pending bank latch, not the committed GS tone.
        if(settings.bankSelect!=MCU_Read(cpu,0xab66+part)
            || settings.controls.volume!=MCU_Read(cpu,base+8)
            || settings.controls.pan!=MCU_Read(cpu,base+9)
            || settings.controls.chorus!=MCU_Read(cpu,base+14)
            || settings.controls.reverb!=MCU_Read(cpu,base+15)) {
            std::printf("PARAM channel=%u cc=%u value=%u bank=%u/%u volume=%u/%u pan=%u/%u chorus=%u/%u reverb=%u/%u\n",
                channel+1,cc,value,settings.bankSelect,MCU_Read(cpu,0xab66+part),settings.controls.volume,MCU_Read(cpu,base+8),
                settings.controls.pan,MCU_Read(cpu,base+9),settings.controls.chorus,MCU_Read(cpu,base+14),
                settings.controls.reverb,MCU_Read(cpu,base+15));
            std::fflush(stdout);
            throw std::runtime_error("CC collision changed native parameter side effects");
        }
        const auto expected=MCU_Read(cpu,0xa050+part);
        if(player.portamentoSource(part)!=expected) {
            std::printf("SOURCE mismatch channel=%u cc=%u value=%u H8=%u native=%u\n",channel+1,cc,value,
                expected,player.portamentoSource(part));
            std::fflush(stdout);
            ++mismatches;
        }
        ++checks;
    }
    if(mismatches) throw std::runtime_error("Source-controller dispatch differs from H8");
    std::printf("Source-controller: %u melodic/rhythm CC-number collisions match H8\n",checks);
}
