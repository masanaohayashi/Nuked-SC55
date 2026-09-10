#pragma once
#include "MidiFilePlayer.h"

// Exploratory real-song replay. Allocation differences are observations, not
// an audio-fidelity verdict: H8 dispatch and native dispatch have different latency.
inline int CompareSongAllocation(Emulator& h8,const RomsetInfo& roms,const char* path,bool part16Only=false,bool firstKick=false)
{
    MidiFileData song; std::string error;
    if(!song.load(path,error,false)) throw std::runtime_error(error);
    double kickTime=-1;
    for(const auto& event:song.events)
        if(event.bytes.size()==3 && event.bytes[0]==0x99 && (event.bytes[1]==35 || event.bytes[1]==36) && event.bytes[2]) {
            kickTime=event.seconds;
            std::printf("FIRST_KICK t=%.6f key=%u velocity=%u\n",kickTime,event.bytes[1],event.bytes[2]); break;
        }
    if(firstKick && kickTime<0) throw std::runtime_error("No channel10 kick found");
    unsigned part16Notes=0;
    for(const auto& event:song.events)
        if(event.bytes.size()==3 && event.bytes[0]==0x9f && event.bytes[2]!=0 && ++part16Notes<=10)
            std::printf("PART16 note=%u t=%.6f key=%u velocity=%u\n",part16Notes,event.seconds,event.bytes[1],event.bytes[2]);
    const auto& raw=roms.rom_data;
    const auto& r1=raw[size_t(RomLocation::ROM1)];
    const auto& r2=raw[size_t(RomLocation::ROM2)];
    sc55::SoundData sounds;
    if(!sounds.loadEncoded(sc55::ImportSoundData(r1,r2))) throw std::runtime_error("sound import failed");
    auto pcm=std::make_unique<pcm_t>(); pcm->is_mk1=true;
    std::copy_n(h8.GetPCM().waverom1,sizeof(pcm->waverom1),pcm->waverom1);
    std::copy_n(h8.GetPCM().waverom2,sizeof(pcm->waverom2),pcm->waverom2);
    std::copy_n(h8.GetPCM().waverom3,sizeof(pcm->waverom3),pcm->waverom3);
    sc55::NativeMelodicPlayer player(sounds,*pcm,sc55::ImportSystemDefaults(r1,r2),
        sc55::ImportRhythmPresets(r1,r2),sc55::ImportMelodicPresets(r1,r2),sc55::ImportEffectsTables(r1,r2));
    auto& cpu=h8.GetMCU();
    while(cpu.cycles<120000000) h8.Step();
    player.renderFrames(16384);
    // Diagnostic-only playback start phase: reproduce time spent idle before Play.
    if(firstKick) if(const auto* value=std::getenv("SC55_KICK_IDLE_FRAMES")) {
        const auto frames=std::min<unsigned long>(std::strtoul(value,nullptr,10),32000);
        const auto end=cpu.cycles+frames*625;
        while(cpu.cycles<end) h8.Step();
        player.renderFrames(frames);
        std::printf("PLAYBACK_IDLE_FRAMES %lu\n",frames);
    }
    const auto base=cpu.cycles;
    struct Attacks {
        pcm_t* pcm; const char* name; uint64_t base;
        uint64_t targetGain=0;
        uint32_t targetAddress=0;
        unsigned silentAttacks=0;
        double probeTime=-1;
        bool allKicks=false;
        struct KickGain { double seconds; uint64_t gain; };
        std::vector<KickGain> kicks;
        struct Voice { uint64_t begin=0; uint32_t address=0; unsigned age=0; uint64_t peakGain=0; };
        std::array<Voice,24> voices{};
        unsigned starts=0;
        static void sample(void* context,const AudioFrame<int32_t>&) {
            auto& self=*static_cast<Attacks*>(context); const auto& p=*self.pcm;
            const auto active=p.voice_mask&p.voice_mask_pending;
            for(unsigned slot=0;slot<24;++slot) {
                auto& voice=self.voices[slot];
                // PCM's key-on is key && !okey, not an edge in the mask:
                // installing a reused slot clears its latched mode bit 5.
                if((active&(1u<<slot)) && !(p.ram2[slot][7]&0x20)) {
                    voice={p.cycles-self.base,p.ram1[slot][4],0,0}; ++self.starts;
                }
                if((active&(1u<<slot)) && voice.age<(self.probeTime>=0?1024u:128u)) {
                    voice.peakGain=std::max(voice.peakGain,uint64_t(p.ram2[slot][9])*p.ram2[slot][10]);
                    if(self.probeTime>=0 && voice.address==0x6eca0 && voice.age==32)
                        self.kicks.push_back({voice.begin/20000000.0,uint64_t(p.ram2[slot][9])*p.ram2[slot][10]});
                    if(self.probeTime>=0 && ((self.allKicks && voice.address==0x6eca0) || (voice.begin/20000000.0>=self.probeTime && voice.begin/20000000.0<self.probeTime+0.04))
                        && (voice.age<8 || voice.age%32==0 || voice.age==127))
                        std::printf("KICK_WINDOW %s t=%.6f slot=%u age=%u start=%x pos=%x pitch=%x commands=%x,%x,%x levels=%x,%x,%x history=%x,%x,%x\n",
                            self.name,voice.begin/20000000.0,slot,voice.age,voice.address,p.ram1[slot][4],p.ram2[slot][0],
                            p.ram2[slot][3],p.ram2[slot][4],p.ram2[slot][5],p.ram2[slot][9],p.ram2[slot][10],p.ram2[slot][11],p.ram1[slot][5],p.ram1[slot][1],p.ram1[slot][3]);
                    if(++voice.age==128) {
                        self.silentAttacks+=voice.peakGain==0;
                        if(voice.begin>=uint64_t(17.518*20000000) && voice.begin<uint64_t(17.56*20000000)
                            && (voice.address&0xfff00)==0xcd900) {
                            self.targetGain=voice.peakGain; self.targetAddress=voice.address;
                        }
                        if(voice.begin>=uint64_t(17.4*20000000) && voice.begin<uint64_t(17.7*20000000))
                        std::printf("ATTACK %s t=%.6f slot=%u start=%x position=%x gain128=%llu\n",
                            self.name,voice.begin/20000000.0,slot,voice.address,p.ram1[slot][4],
                            (unsigned long long)voice.peakGain);
                    }
                }
            }
        }
    } h8Attacks{&h8.GetPCM(),"H8",h8.GetPCM().cycles},nativeAttacks{pcm.get(),"CPP",pcm->cycles};
    if(firstKick) h8Attacks.probeTime=nativeAttacks.probeTime=kickTime-0.0001;
    const bool allKicks=firstKick && std::getenv("SC55_KICK_ALL")!=nullptr;
    h8Attacks.allKicks=nativeAttacks.allKicks=allKicks;
    h8.GetPCM().enable_oversampling=false;
    h8.SetSampleCallback(Attacks::sample,&h8Attacks);
    pcm->output_context=&nativeAttacks; pcm->output_sample=Attacks::sample;
    size_t next=0; unsigned mismatches=0,full=0,notes=0,panelStep=0;
    const auto limit=uint64_t(std::min(song.totalSeconds(),firstKick&&!allKicks?kickTime+0.15:part16Only?18.0:60.0)*32000);
    std::printf("SONG %s events=%zu duration=%.3f replay=%.3f\n",path,song.events.size(),song.totalSeconds(),limit/32000.0);
    for(uint64_t frame=0;frame<limit;) {
        // Drive the firmware's actual panel buttons after song initialization.
        // Mute displayed parts 1..15, leaving part16 selected and unmuted.
        if(part16Only && panelStep<60 && frame>=96000+uint64_t(panelStep)*1920) {
            if((panelStep&1)==0) {
                const unsigned operation=panelStep/2;
                const auto button=(operation&1) ? MCU_BUTTON_PART_R : MCU_BUTTON_INST_MUTE;
                cpu.button_pressed.store(1u<<button);
                if(!(operation&1)) {
                    const unsigned displayed=operation/2;
                    const unsigned part=displayed<9 ? displayed+1 : displayed==9 ? 0 : displayed;
                    player.toggleMute(part,false);
                }
            } else cpu.button_pressed.store(0);
            ++panelStep;
        }
        while(next<song.events.size() && uint64_t(song.events[next].seconds*32000)<=frame) {
            const auto& bytes=song.events[next++].bytes;
            if(part16Only && panelStep==60)
                for(unsigned part=0;part<16;++part)
                    if(player.partMuted(part)!=(part!=15)
                        || (!(MCU_Read16(cpu,0x804a+0x70*part)&0x0200))!=(part!=15))
                        throw std::runtime_error("Panel mute setup differs from requested parts 1..15");
            h8.PostMIDI(bytes);
            if(player.push(bytes)!=bytes.size()) throw std::runtime_error("song MIDI queue overflow");
            notes+=bytes.size()==3 && (bytes[0]&0xf0)==0x90 && bytes[2]!=0;
        }
        uint64_t until=std::min(frame+128,limit);
        if(next<song.events.size()) until=std::min(until,std::max(frame+1,uint64_t(song.events[next].seconds*32000)));
        while(cpu.cycles<base+until*625) h8.Step();
        player.renderFrames(until-frame); frame=until;
        if(player.failed()) throw std::runtime_error("native song playback failed");
        unsigned a=0,b=0; bool same=true;
        for(unsigned part=0;part<16;++part) {
            const auto expected=MCU_Read(cpu,0xa1f0+part);
            a+=expected; b+=player.partVoiceCount(part);
            same &= expected==player.partVoiceCount(part);
        }
        full+=a==24;
        if(!same && ++mismatches<=20) {
            std::printf("ALLOCATION t=%.6f events=%zu voices=%u/%u parts H8/CPP:",frame/32000.0,next,a,b);
            for(unsigned part=0;part<16;++part)
                if(MCU_Read(cpu,0xa1f0+part)!=player.partVoiceCount(part))
                    std::printf(" %u:%u/%u",part,MCU_Read(cpu,0xa1f0+part),player.partVoiceCount(part));
            std::puts("");
        }
    }
    std::printf("SONG observations: notes=%u full_capacity_samples=%u allocation_difference_samples=%u queued=%zu\n",
        notes,full,mismatches,size_t(player.queuedEvents()));
    std::printf("PCM key-on edges: H8=%u CPP=%u\n",h8Attacks.starts,nativeAttacks.starts);
    h8.SetSampleCallback(nullptr,nullptr);
    if(firstKick) {
        if(h8Attacks.kicks.empty() || h8Attacks.kicks.size()!=nativeAttacks.kicks.size())
            throw std::runtime_error("Kick attack count differs from H8 (this probe expects 55KTIZKE kick sample)");
        for(size_t i=0;i<h8Attacks.kicks.size();++i) {
            const auto& a=h8Attacks.kicks[i]; const auto& b=nativeAttacks.kicks[i];
            if(std::abs(a.seconds-b.seconds)>0.04 || a.gain!=b.gain) {
                std::printf("KICK_ATTACK_FAIL t=%.6f H8=%llu CPP=%llu\n",b.seconds,
                    (unsigned long long)a.gain,(unsigned long long)b.gain);
                throw std::runtime_error("Kick first-millisecond gain differs from H8");
            }
        }
        std::printf("PASS: %zu kick attacks match H8 first-millisecond gain\n",h8Attacks.kicks.size());
    }
    std::printf("Silent attack windows H8/CPP: %u/%u (not all silent windows are erroneous)\n",h8Attacks.silentAttacks,nativeAttacks.silentAttacks);
    if(part16Only) {
        if(panelStep!=60 || h8Attacks.targetAddress!=0xcd940 || nativeAttacks.targetAddress!=h8Attacks.targetAddress
            || !h8Attacks.targetGain || nativeAttacks.targetGain!=h8Attacks.targetGain)
            throw std::runtime_error("GATCHA55 part16 note8 sample start/attack differs from H8 after panel mute");
        std::puts("PASS: GATCHA55 part16 note8 attack and sample start match H8 after initialization and panel mute");
    }
    return 0;
}
