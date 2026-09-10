#pragma once
#include "sc55_native_player.h"

inline void VerifyBulkSystem(Emulator& emu,const RomsetInfo& roms)
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
    std::optional<sc55::SystemDefaults> expected;
    unsigned writes=0;
    const auto run=[&](unsigned cycles) {
        const auto end=cpu.cycles+cycles;
        while(cpu.cycles<end) {
            if(expected && cpu.cp==4 && cpu.pc==0x1617) {
                for(unsigned i=0;i<expected->bytes.size();++i)
                    if(expected->bytes[i]!=MCU_Read(cpu,0x8000+i))
                        throw std::runtime_error("Bulk48 wire decoding differs from H8");
                expected.reset(); ++writes;
            }
            emu.Step();
        }
        player.renderFrames(cycles/625);
        if(player.failed()) throw std::runtime_error("Bulk48 native player failed");
    };
    const auto compare=[&] {
        sc55::SystemDefaults current;
        for(unsigned i=0;i<current.bytes.size();++i) current.bytes[i]=MCU_Read(cpu,0x8000+i);
        for(unsigned i=0;i<current.bytes.size();++i) {
            const auto actual=player.readSystemConfiguration(i);
            if(!actual || *actual!=current.bytes[i]) {
                std::printf("BULK projection offset=%03x H8=%02x native=%d\n",i,current.bytes[i],actual ? int(*actual) : -1);
                throw std::runtime_error("Native complete configuration projection differs from H8");
            }
        }
        if(player.readSystemConfiguration(0x748)) throw std::runtime_error("Out-of-range configuration read accepted");
        const auto h8=current.parts(); const auto& native=player.partSettings();
        const auto h8Controllers=current.controllers();
        const auto master=current.master(); const auto& actual=player.masterControls();
        if(master.tune!=actual.tune || master.volume!=actual.volume || master.keyShift!=actual.keyShift
            || master.pan!=actual.pan || master.portamentoController!=actual.portamentoController)
            throw std::runtime_error("Bulk48 master settings differ");
        for(unsigned i=0;i<16;++i) {
            const auto& a=native.parts[i]; const auto& b=h8.parts[i];
            if(a.bank!=b.bank || a.controls.program!=b.controls.program || a.controls.volume!=b.controls.volume
                || a.controls.pan!=b.controls.pan || a.controls.chorus!=b.controls.chorus || a.controls.reverb!=b.controls.reverb
                || a.keyShift!=b.keyShift || a.fineTune!=b.fineTune || a.scale!=b.scale || a.controls.tone.values!=b.controls.tone.values
                || a.keyRange.low!=b.keyRange.low || a.keyRange.high!=b.keyRange.high
                || a.velocity.depth!=b.velocity.depth || a.velocity.offset!=b.velocity.offset
                || native.routing[i].channel!=h8.routing[i].channel || native.routing[i].noteFlags!=h8.routing[i].noteFlags
                || native.routing[i].flags!=h8.routing[i].flags) {
                std::printf("BULK settings mismatch part=%u\n",i);
                throw std::runtime_error("Bulk48 part settings differ");
            }
            if(a.controls.expression!=MCU_Read(cpu,0xab36+i))
                throw std::runtime_error("Bulk48 incorrectly reset dynamic expression");
#if defined(SC55_NATIVE_IO_AUDIT)
            const auto& ac=player.controllerSettingsAudit().parts[i]; const auto& bc=h8Controllers.parts[i];
            if(ac.assignedControllers!=bc.assignedControllers || ac.sensitivity!=bc.sensitivity
                || ac.sourceSensitivity!=bc.sourceSensitivity)
                throw std::runtime_error("Bulk48 modulation settings differ");
#endif
            if(player.capacityPolicy().reserves[i]!=current.bytes[0x18+i])
                throw std::runtime_error("Bulk48 reserve settings differ");
        }
        const auto& effects=player.effectsSettings();
        if(!std::equal(effects.reverb.begin(),effects.reverb.end(),current.bytes.begin()+0x2b)
            || !std::equal(effects.chorus.begin(),effects.chorus.end(),current.bytes.begin()+0x33)
            || effects.reverbMacro!=current.bytes[0x2a] || effects.chorusMacro!=current.bytes[0x32]
            || player.capacityPolicy().startPartControl!=current.bytes[0x28])
            throw std::runtime_error("Bulk48 effects/priority settings differ");
    };
    const auto send=[&](std::vector<uint8_t> payload) {
        expected.emplace();
        for(unsigned i=0;i<expected->bytes.size();++i) expected->bytes[i]=MCU_Read(cpu,0x8000+i);
        if(!sc55::BulkSystemData::write(payload,[&](unsigned offset,uint8_t value) {expected->bytes[offset]=value;}))
            throw std::runtime_error("Invalid test payload");
        std::vector<uint8_t> packet{0xf0,0x41,0x10,0x42,0x12};
        unsigned sum=0;
        for(auto byte:payload) {packet.push_back(byte); sum+=byte;}
        packet.push_back(uint8_t(-sum)&127); packet.push_back(0xf7);
        emu.PostMIDI(packet);
        // Fragment reception without changing the message timestamp.
        for(auto byte:packet) if(player.push(std::span(&byte,1))!=1) throw std::runtime_error("Bulk ingress rejected byte");
        run(4000000);
        if(expected) throw std::runtime_error("H8 did not reach bulk reapplication");
        compare();
    };
    compare();
    const uint8_t note[]{0xb0,11,37,0x90,60,100}; emu.PostMIDI(note); player.push(note); run(4000000);
    send({0x48,0,4,5,13}); // Master volume93, with a live voice.
    std::array<uint8_t,0x70> part{};
    for(unsigned i=0;i<part.size();++i) part[i]=MCU_Read(cpu,0x80b8+i);
    part[1]=80; part[6]=67; part[8]=91; part[9]=42; part[0x10]=62; part[0x1a]=65;
    part[0x26]=17; part[0x27]=18;
    for(unsigned i=0;i<6;++i) part[0x28+12*i]=uint8_t(66+i);
    for(auto field:sc55::UninterpretedSystemSettings::partOffsets) part[field]=uint8_t(field+11);
    for(unsigned start=0;start<part.size();start+=64) {
        const auto offset=0xb8+start;
        std::vector<uint8_t> payload{0x48,uint8_t(offset/64),uint8_t((offset%64)*2)};
        for(unsigned i=start;i<std::min<unsigned>(start+64,part.size());++i) {
            payload.push_back(part[i]>>4); payload.push_back(part[i]&15);
        }
        send(payload);
    }
    send({0x48,1,0}); // Empty packet writes EF into an unused configuration byte.
    send({0x48,1,1,3}); // Odd address floors; unmatched high byte uses FF.
    send({0x48,29,14,4,0,5,0}); // Last byte is written, overflow suffix clipped.
    std::vector<uint8_t> reserve{0x48,0,0x30};
    for(unsigned partIndex=0;partIndex<16;++partIndex) {reserve.push_back(0); reserve.push_back(partIndex>=1 && partIndex<=3 ? 8 : 0);}
    reserve.push_back(0); reserve.push_back(2); send(reserve);
    std::vector<uint8_t> effect{0x48,0,0x54};
    for(unsigned i=0x2a;i<0x3a;++i) {
        const auto value=i==0x2e ? uint8_t(70) : i==0x36 ? uint8_t(50) : MCU_Read(cpu,0x8000+i);
        effect.push_back(value>>4); effect.push_back(value&15);
    }
    send(effect);
    if(writes!=8 || player.rejectedSysEx()) throw std::runtime_error("Bulk48 cases were not all accepted");
    // A bulk master write still reapplies all programs. It must reach the
    // receiver while older note commands remain, not wait for voice draining.
    std::vector<uint8_t> backlog;
    for(uint8_t key=48;key<96;++key) backlog.insert(backlog.end(),{0x90,key,100});
    const uint8_t volume[]{0xf0,0x41,0x10,0x42,0x12,0x48,0,4,1,1,0x32,0xf7};
    backlog.insert(backlog.end(),std::begin(volume),std::end(volume));
    emu.PostMIDI(backlog); player.push(backlog);
    bool h8Applied=false,nativeApplied=false;
    unsigned writeCursor=0,readCursor=0,nativeOutstanding=0;
    const auto limit=cpu.cycles+12000000;
    while(cpu.cycles<limit) {
        if(!h8Applied && MCU_Read(cpu,0x8002)==17) {
            h8Applied=true;
            writeCursor=MCU_Read(cpu,0xaaf8); readCursor=MCU_Read(cpu,0xaaf9);
        }
        emu.Step();
    }
    for(unsigned frame=0;frame<19200;++frame) {
        player.renderFrames(1);
        if(!nativeApplied && player.masterControls().volume==17) {
            nativeApplied=true; nativeOutstanding=unsigned(player.queuedEvents());
        }
    }
    std::printf("Bulk during notes: H8 cursors=%02x/%02x native queued=%u\n",writeCursor,readCursor,nativeOutstanding);
    if(!h8Applied || writeCursor==readCursor || !nativeApplied || !nativeOutstanding)
        throw std::runtime_error("Bulk receiver did not overlap pending voice commands");
    if(player.failed() || player.queuedEvents()) throw std::runtime_error("Bulk backlog did not complete");
    compare();
    const uint8_t reset[]{0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7};
    emu.PostMIDI(reset); player.push(reset); run(20000000);
    if(player.resetPending() || player.completedResets()!=1) throw std::runtime_error("Bulk test reset incomplete");
    compare();
    std::printf("Bulk48 PASS: %u transfers, full1864-byte native readback and reset match H8, 16-part/master reapplication\n",writes);
}
