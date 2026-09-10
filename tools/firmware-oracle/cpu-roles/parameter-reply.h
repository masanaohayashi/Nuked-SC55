#pragma once
#include "sc55_native_player.h"

inline void VerifyParameterReplies(Emulator& emu,const RomsetInfo& roms,bool checkResetTiming=false,bool minimal=false)
{
    const auto& raw=roms.rom_data;
    const auto& r1=raw[size_t(RomLocation::ROM1)];
    const auto& r2=raw[size_t(RomLocation::ROM2)];
    sc55::SoundData sounds;
    if(!sounds.loadEncoded(sc55::ImportSoundData(r1,r2))) throw std::runtime_error("Invalid sound data");
    auto pcm=std::make_unique<pcm_t>(); pcm->is_mk1=true;
    std::copy_n(emu.GetPCM().waverom1,sizeof(pcm->waverom1),pcm->waverom1);
    std::copy_n(emu.GetPCM().waverom2,sizeof(pcm->waverom2),pcm->waverom2);
    std::copy_n(emu.GetPCM().waverom3,sizeof(pcm->waverom3),pcm->waverom3);
    sc55::NativeMelodicPlayer player(sounds,*pcm,sc55::ImportSystemDefaults(r1,r2),
        sc55::ImportRhythmPresets(r1,r2),sc55::ImportMelodicPresets(r1,r2),sc55::ImportEffectsTables(r1,r2));
    auto& cpu=emu.GetMCU();
    const auto boot=cpu.cycles+120000000;
    while(cpu.cycles<boot) emu.Step();
    player.renderFrames(16384);
    bool resetReturned=false;
    unsigned resetEffectNotifications=0;
    const auto send=[&](uint8_t command,std::vector<uint8_t> payload,unsigned runCycles=2000000) {
        std::vector<uint8_t> packet{0xf0,0x41,0x10,0x42,command};
        unsigned sum=0;
        for(auto b:payload) {packet.push_back(b); sum+=b;}
        packet.push_back(uint8_t(-sum)&127); packet.push_back(0xf7);
        emu.PostMIDI(packet);
        for(auto b:packet) if(player.push(std::span(&b,1))!=1) throw std::runtime_error("Native MIDI queue overflow");
        std::vector<uint8_t> output;
        const auto end=cpu.cycles+runCycles;
        while(cpu.cycles<end) {
            if(cpu.cp==4 && (cpu.pc==0x12b8 || cpu.pc==0x12be)) ++resetEffectNotifications;
            if(cpu.cp==4 && cpu.pc==0x379f) resetReturned=true;
            // Diagnostic only: original emulator misses the ready IRQ when
            // TIE is enabled with TDRE already set. Observe real H8 TX bytes.
            if((cpu.dev_register[DEV_SCR]&0xa0)==0xa0 && (cpu.dev_register[DEV_SSR]&0x80))
                MCU_Interrupt_SetRequest(cpu,INTERRUPT_SOURCE_UART_TX,1);
            const auto before=MCU_Read16(cpu,0xabfc);
            emu.Step();
            const auto after=MCU_Read16(cpu,0xabfc);
            const unsigned count=(after-before)&255;
            if(before<256 && after<256 && count<=2)
                for(unsigned i=0;i<count;++i) output.push_back(MCU_Read(cpu,0xa5f8+((before+i)&255)));
        }
        player.renderFrames(runCycles/625);
        if(player.failed()) throw std::runtime_error("Native player failed during RQ1");
        return output;
    };
    unsigned verified=0;
    const auto request=[&](unsigned page,unsigned key,unsigned length,unsigned sizeByte=5,unsigned region=0x40,unsigned requestBytes=6) {
        std::vector<uint8_t> payload{uint8_t(region),uint8_t(page),uint8_t(key),0,0,0};
        payload[sizeByte]=uint8_t(length);
        payload.resize(requestBytes);
        const auto h8=send(0x11,payload);
        sc55::ParameterReply native;
        if(!player.popParameterReply(native) || h8.size()!=native.size
            || !std::equal(h8.begin(),h8.end(),native.bytes.begin())) {
            std::printf("RQ1 mismatch page=%02x key=%02x length=%u H8:",page,key,length);
            for(auto b:h8) std::printf(" %02x",b);
            std::printf(" native:"); for(unsigned i=0;i<native.size;++i) std::printf(" %02x",native.bytes[i]);
            std::printf("\n");
            throw std::runtime_error("RQ1 packet differs from H8");
        }
        ++verified;
    };
    if(minimal) {
        send(0x12,{0x40,0,0x7f,0});
        if(!resetReturned || resetEffectNotifications!=2)
            throw std::runtime_error("H8 reset did not notify FX and return in this fixture");
        if(player.resetPending() || player.effectsSettled() || player.completedResets()!=1)
            throw std::runtime_error("Native reset must complete independently of FX ramps");
        const uint8_t note[]{0x90,60,100};
        emu.PostMIDI(note); player.push(note);
        request(1,0,16);
        if(player.freeVoices()==24 || player.freeVoices()!=MCU_Read(cpu,0xa3c1))
            throw std::runtime_error("Following note did not resume like H8 during FX transition");
        send(0x12,{0x40,0,0x7f,0}); request(1,0,16);
        if(player.resetPending() || player.completedResets()!=2 || player.freeVoices()!=24
            || MCU_Read(cpu,0xa3c1)!=24)
            throw std::runtime_error("Active-voice reset did not drain before resuming MIDI");
        std::printf("Reset readback and following note match H8; active-voice reset drains correctly\n"); return;
    }
    for(unsigned key:{0u,4u,5u,6u,0x7eu,0x7fu}) request(0,key,key==0 ? 4 : 1);
    for(unsigned page=0x30;page<0x40;++page) {
        request(page,0,32);
        request(page,0x20,32);
    }
    // These two information handlers ignore size, unlike normal records.
    for(unsigned length:{0u,1u,127u}) {
        request(0x30,0,length);
        request(0x32,0x20,length);
    }
    for(unsigned key:{0u,0x20u}) request(0x30,key,32,5,0x40,3);
    // Bulk can change the stored reset byte without executing GS Reset.
    // RQ1 must return that byte, not a hardcoded zero or trigger a reset.
    send(0x12,{0x48,0,8,2,5}); request(0,0x7f,1);
    if(player.completedResets()!=0) throw std::runtime_error("Reading reset register triggered reset");
    send(0x12,{0x40,0,4,93}); request(0,4,1);
    const auto midi=[&](std::initializer_list<uint8_t> message) {
        emu.PostMIDI(std::span(message.begin(),message.size()));
        if(player.push(std::span(message.begin(),message.size()))!=message.size())
            throw std::runtime_error("Bank selection queue full");
        request(0x11,0,2);
    };
    midi({0xb0,0,8});  // Pending bank must not change the current GS tone.
    send(0x12,{0x48,0,4,5,13}); request(0x11,0,2); // Bulk reapply must use stored tone.
    if(player.partSettings().parts[1].bankSelect!=MCU_Read(cpu,0xab67))
        throw std::runtime_error("Bulk bank latch differs from H8");
    midi({0xb0,0,8});
    midi({0xc0,0});    // Program Change commits it.
    midi({0xb0,0,127});
    cpu.button_pressed.store(1u<<MCU_BUTTON_INST_R);
    player.adjustPart(1,sc55::PartParameter::program,1);
    // Physical panel scan precedes this readback; sending RQ1 immediately
    // would let the MIDI task reply before the key has been scanned.
    const auto panelEnd=cpu.cycles+2000000;
    while(cpu.cycles<panelEnd) emu.Step();
    player.renderFrames(3200);
    request(0x11,0,2);
    cpu.button_pressed.store(0);
    if(player.partSettings().parts[1].bankSelect!=MCU_Read(cpu,0xab67))
        throw std::runtime_error("Panel bank latch differs from H8");
    midi({0xc0,0});    // Includes fallback/unsupported bank resolution.
    midi({0xb0,0,0});
    midi({0xc0,0});
    for(unsigned part:{0u,1u,15u}) {
        const auto page=0x10+part;
        request(page,0,2); request(page,2,1);
        for(unsigned key=3;key<=0x17;++key) request(page,key,key==0x17 ? 2 : 1);
        for(unsigned key=0x19;key<=0x22;++key) request(page,key,1);
        for(unsigned key=0x30;key<=0x37;++key) request(page,key,1);
        request(page,0x40,12);
        for(unsigned source=0;source<6;++source)
            for(unsigned column=0;column<11;++column) request(0x20+part,source*16+column,1);
    }
    send(0x12,{0x40,0x11,0x19,91}); request(0x11,0x19,1);
    send(0x12,{0x40,0x11,0x17,9,3}); request(0x11,0x17,2);
    request(0,4,1,3); request(0,4,1,4);
    request(1,0,16); request(1,0x10,16); request(1,0x20,1);
    for(unsigned key=0x30;key<=0x3f;++key) if(key!=0x37) request(1,key,1);
    send(0x12,{0x40,1,0,'N','A','T','I','V','E',0,1,2,3,4,5,6,7,8,9}); request(1,0,16);
    send(0x12,{0x40,1,0x34,70}); request(1,0x34,1);
    send(0x12,{0x40,1,0x3a,50}); request(1,0x3a,1);
    for(unsigned map=0;map<2;++map) {
        request(map*16,0,12,5,0x41);
        for(unsigned field=1;field<=8;++field)
            for(unsigned key:{0u,36u,127u}) request(map*16+field,key,1,5,0x41);
    }
    send(0x12,{0x41,2,36,73}); request(2,36,1,5,0x41);
    send(0x12,{0x41,0x18,127,1}); request(0x18,127,1,5,0x41);
    send(0x12,{0x41,0x10,0,'N','A','T','I','V','E',0,1,2,3,4,5}); request(0x10,0,12,5,0x41);
    send(0x12,{0x48,0,0x10,5,8}); request(1,0,16);
    // Reset drains existing effects before accepting subsequent MIDI. This
    // fixture checks readback after completion, not reset latency parity.
    send(0x12,{0x40,0,0x7f,0},checkResetTiming ? 2000000 : 20000000);
    if(!checkResetTiming && player.resetPending()) throw std::runtime_error("Reset did not finish before readback");
    request(1,0,16); request(0x10,0,12,5,0x41); request(0,0x7f,1);
    for(auto payload:std::array<std::array<uint8_t,6>,8>{{
        {0x40,0,4,0,0,2},{0x40,0x11,0,0,0,1},
        {0x40,0x11,0x17,0,0,1},{0x40,0x11,0x40,0,0,11},
        {0x40,1,0,0,0,15},{0x40,1,0x10,0,0,15},
        {0x41,0,0,0,0,11},{0x41,2,36,0,0,2}}}) {
        const auto h8=send(0x11,{payload.begin(),payload.end()});
        sc55::ParameterReply native;
        if(player.popParameterReply(native) || std::find(h8.begin(),h8.end(),0xf7)!=h8.end())
            throw std::runtime_error("Invalid RQ1 size produced a response");
    }
    if(player.droppedParameterReplies()) throw std::runtime_error("RQ1 response queue overflow");
    // Bounded native reply staging: retain earlier packets, drop the newest
    // on overflow. This is not yet the firmware's paced TX task.
    for(unsigned i=0;i<17;++i) send(0x11,{0x40,0,4,0,0,1});
    unsigned drained=0; sc55::ParameterReply reply;
    while(player.popParameterReply(reply)) ++drained;
    if(drained!=16 || player.droppedParameterReplies()!=1)
        throw std::runtime_error("RQ1 bounded queue overflow policy failed");
    std::printf("RQ1 %u complete response packets match H8 TX, 8 invalid sizes rejected; bounded queue checked\n",verified);
}
