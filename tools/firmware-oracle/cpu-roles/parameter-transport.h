#pragma once
#include "sc55_native_player.h"

inline void VerifyParameterTransport(Emulator& emu,const RomsetInfo& roms)
{
    const auto& raw=roms.rom_data;
    const auto& r1=raw[size_t(RomLocation::ROM1)];const auto& r2=raw[size_t(RomLocation::ROM2)];
    sc55::SoundData data;
    if(!data.loadEncoded(sc55::ImportSoundData(r1,r2))) throw std::runtime_error("Invalid sound data");
    auto pcm=std::make_unique<pcm_t>();pcm->is_mk1=true;
    std::copy_n(emu.GetPCM().waverom1,sizeof(pcm->waverom1),pcm->waverom1);
    std::copy_n(emu.GetPCM().waverom2,sizeof(pcm->waverom2),pcm->waverom2);
    std::copy_n(emu.GetPCM().waverom3,sizeof(pcm->waverom3),pcm->waverom3);
    sc55::NativeMelodicPlayer player(data,*pcm,sc55::ImportSystemDefaults(r1,r2),
        sc55::ImportRhythmPresets(r1,r2),sc55::ImportMelodicPresets(r1,r2),sc55::ImportEffectsTables(r1,r2));
    auto& cpu=emu.GetMCU();const auto boot=cpu.cycles+120000000;
    while(cpu.cycles<boot) emu.Step();
    player.renderFrames(16384);
    const std::vector<uint8_t> request{0xf0,0x41,0x10,0x42,0x11,0x40,0x11,0x19,0,0,1,0x15,0xf7};
    // The normal no-output product must ignore requests without filling the
    // eight-packet capture queue or delaying following SysEx/CC reception.
    for(unsigned i=0;i<16;++i) {
        if(player.push(request)!=request.size()) throw std::runtime_error("No-output RQ1 queue full");
        player.renderFrames(1600);
    }
    const uint8_t volume[]{0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0x19,87,0x3f,0xf7};
    // No-output is a product policy, not an H8 TX timing comparison. Exercise
    // it while voice preparation is busy, not only after an idle request.
    const uint8_t notes[]{0x90,60,100,0x90,64,100,0x90,67,100};
    player.push(notes);player.renderFrames(1);
    player.push(request);player.push(volume);player.renderFrames(1);
    if(player.partSettings().parts[1].controls.volume!=87)
        throw std::runtime_error("Disabled RQ1 blocked DT1 during voice preparation");
    const uint8_t allSoundOff[]{0xb0,120,0};
    player.push(allSoundOff);player.renderFrames(16000);
    player.push(volume);player.renderFrames(1600);
    sc55::ParameterReply unused;
    if(player.failed() || player.parameterReplyPending() || player.popParameterReply(unused)
        || player.droppedParameterReplies() || player.partSettings().parts[1].controls.volume!=87)
        throw std::runtime_error("Disabled output blocked SysEx reception or generated replies");
    // Restore the fixture before its independent, explicitly connected test.
    const uint8_t resetVolume[]{0xb0,7,100};player.push(resetVolume);player.renderFrames(1600);
    std::puts("Native no-output mode: repeated RQ1 ignored, DT1 volume applied, no pending/dropped replies PASS");
    if(!player.setParameterOutputConnected(true)) throw std::runtime_error("Cannot connect parameter transport");
    std::vector<uint8_t> input=request;
    input.insert(input.end(),{0xb0,7,93,0x90,60,100});
    emu.PostMIDI(input);player.push(input);
    std::vector<uint8_t> tx;
    const auto run=[&](unsigned cycles,bool drain) {
        const auto end=cpu.cycles+cycles;
        while(cpu.cycles<end) {
            // Same diagnostic-only ready IRQ supplementation as other TX tests.
            if(drain && (cpu.dev_register[DEV_SCR]&0xa0)==0xa0 && (cpu.dev_register[DEV_SSR]&0x80))
                MCU_Interrupt_SetRequest(cpu,INTERRUPT_SOURCE_UART_TX,1);
            const auto before=MCU_Read16(cpu,0xabfc);emu.Step();const auto after=MCU_Read16(cpu,0xabfc);
            const unsigned count=(after-before)&255;
            if(before<256 && after<256 && count<=2)
                for(unsigned i=0;i<count;++i) tx.push_back(MCU_Read(cpu,0xa5f8+((before+i)&255)));
        }
    };
    run(2000000,false);player.renderFrames(3200);
    if(!player.parameterReplyPending() || player.partSettings().parts[1].controls.volume!=100
        || MCU_Read(cpu,0x80c0)!=100 || player.freeVoices()!=24 || MCU_Read(cpu,0xa3c1)!=24)
        throw std::runtime_error("Following MIDI overtook an unsent parameter response");
    sc55::ParameterReply reply;
    if(player.completeParameterReplyTransmission() || player.setParameterOutputConnected(false)
        || player.popParameterReply(reply)) throw std::runtime_error("Pending transport escaped its completion contract");
    if(!player.takeParameterReply(reply) || tx.size()!=reply.size
        || !std::equal(tx.begin(),tx.end(),reply.bytes.begin()))
        throw std::runtime_error("Transport packet differs from H8 TX");
    if(player.takeParameterReply(reply)) throw std::runtime_error("Packet emitted twice before completion");
    // Input remains enabled for ordinary RQ1, unlike normal bulk transfer.
    const uint8_t during[]{0xb0,10,51};emu.PostMIDI(during);player.push(during);
    run(2000000,false);player.renderFrames(3200);
    if(player.partSettings().parts[1].controls.volume!=100 || player.partSettings().parts[1].controls.pan!=64
        || MCU_Read(cpu,0x80c0)!=100 || MCU_Read(cpu,0x80c1)!=64)
        throw std::runtime_error("Dequeuing response resumed MIDI before transmission completed");
    if(!player.completeParameterReplyTransmission() || player.completeParameterReplyTransmission())
        throw std::runtime_error("Transmission completion was not single-use");
    run(4000000,true);player.renderFrames(6400);
    const auto& settings=player.partSettings().parts[1].controls;
    if(player.failed() || player.parameterReplyPending() || settings.volume!=93 || settings.pan!=51
        || MCU_Read(cpu,0x80c0)!=93 || MCU_Read(cpu,0x80c1)!=51
        || player.freeVoices()==24 || player.freeVoices()!=MCU_Read(cpu,0xa3c1))
        throw std::runtime_error("Queued MIDI did not resume in order after parameter TX");
    if(!player.setParameterOutputConnected(false)) throw std::runtime_error("Cannot disconnect idle transport");
    std::puts("Parameter transport: H8 packet, TX wait, retained input, single completion and following note PASS");
}
