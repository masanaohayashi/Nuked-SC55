#pragma once
#include "sc55_voice_control.h"

inline int verifyPcmBoundary(const char* directory)
{
    common::LoadRomsetResult roms;
    if(common::LoadRomset(directory,{},common::RomLoader::Hashing,{},roms)!=common::LoadRomsetError{}
        || roms.picked_name!="mk1-v1.21") throw std::runtime_error("Expected v1.21 ROMs");
    auto emu=std::make_unique<Emulator>();
    if(!emu->Init({}) || !emu->LoadRoms(roms.romset,roms.romset_info))
        throw std::runtime_error("Cannot initialize boundary oracle");
    emu->Reset(); auto& cpu=emu->GetMCU(); auto& pcm=emu->GetPCM();
    sc55::PitchConversion conversion;
    unsigned checks=0;
    for(unsigned variant=0;variant<24;++variant) {
        const unsigned slot=variant%24, peer=(slot+1)%24;
        const auto base=MCU_Read16(cpu,0x676a+2*slot);
        sc55::VoiceControlState voice{sc55::EnvelopeRunner({},{})};
        voice.lifecycle.stages.fill(variant%3==2?14:4);
        voice.lifecycle.cached16=0x1234; voice.lifecycle.cached18=0x2345;
        voice.stopAtSampleEnd=variant%3==1;
        voice.alternatePitchReference=23456;
        voice.pitch.glide.pitch.accumulator=uint32_t(variant*3456);
        voice.pitch.glide.pitch.correction={77,uint16_t(variant&1?0xff00:0x123)};
        sc55::VoiceControlInputs inputs; inputs.pitchReference=12345;
        sc55::VoiceLinks links; links.first[slot]=uint8_t(peer); links.second[peer]=uint8_t(slot);
        for(unsigned n=0;n<24;++n) {
            MCU_Write(cpu,0xcac4+n,links.first[n]); MCU_Write(cpu,0xcadc+n,links.second[n]);
        }
        for(unsigned n=0;n<3;++n) MCU_Write16(cpu,base+2*n,voice.lifecycle.stages[n]);
        MCU_Write(cpu,base-17,voice.stopAtSampleEnd?2:0);
        MCU_Write16(cpu,base-2,uint16_t(slot));
        MCU_Write16(cpu,base+0x1a,voice.lifecycle.cached16);
        MCU_Write16(cpu,base+0x1e,voice.lifecycle.cached18);
        const auto put24=[&](unsigned high,unsigned low,uint32_t value) {
            MCU_Write(cpu,base+high,uint8_t(value>>16)); MCU_Write16(cpu,base+low,uint16_t(value));
        };
        put24(0x29,0x3e,inputs.pitchReference);
        put24(0x2a,0x40,voice.alternatePitchReference);
        put24(0x2d,0x46,voice.pitch.glide.pitch.accumulator);
        MCU_Write(cpu,base+0xa4,77);
        MCU_Write16(cpu,base+0xa6,voice.pitch.glide.pitch.correction.offset);
        pcm.ram2[slot][9]=uint16_t(variant&1?100:200);
        pcm.ram2[slot][10]=uint16_t(variant&1?200:100);
        auto nativePcm=std::make_unique<pcm_t>(pcm);
        cpu.cp=cpu.dp=cpu.ep=0; cpu.br=0xe0; cpu.sr=0;
        cpu.pc=0x2928; cpu.r[1]=uint16_t(slot); cpu.r[7]=0xff80;
        unsigned steps=0;
        while(cpu.pc!=0x54ae) {
            if(++steps>200) throw std::runtime_error("PCM boundary escaped ROM routine");
            const auto opcode=MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
        }
        if(!sc55::HandleVoicePcmBoundary(slot,voice,inputs,links,conversion,
            [&](uint8_t a){return PCM_Read(*nativePcm,a);},
            [&](uint8_t a,uint8_t v){PCM_Write(*nativePcm,a,v);}))
            throw std::runtime_error("Native PCM boundary rejected");
        bool same=true;
        for(unsigned n=0;n<3;++n) same &= voice.lifecycle.stages[n]==MCU_Read16(cpu,base+2*n);
        same &= voice.lifecycle.cached16==MCU_Read16(cpu,base+0x1a)
            && voice.lifecycle.cached18==MCU_Read16(cpu,base+0x1e)
            && inputs.pitchReference==((uint32_t(MCU_Read(cpu,base+0x29))<<16)|MCU_Read16(cpu,base+0x3e))
            && voice.pitch.glide.pitch.correction.source==MCU_Read(cpu,base+0xa4);
        for(unsigned n=0;n<24;++n)
            same &= links.first[n]==MCU_Read(cpu,0xcac4+n) && links.second[n]==MCU_Read(cpu,0xcadc+n);
        for(unsigned n=0;n<16;++n) same &= nativePcm->ram2[slot][n]==pcm.ram2[slot][n];
        if(!same) {
            std::fprintf(stderr,"PCM boundary mismatch variant=%u\n",variant);
            throw std::runtime_error("Native PCM boundary differs from ROM");
        }
        ++checks;
    }
    std::printf("PCM boundary: %u ROM/native cases matched\n",checks);
    return 0;
}
