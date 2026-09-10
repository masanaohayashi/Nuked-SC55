#pragma once
#include "MidiFilePlayer.h"
#include <map>
#include <tuple>

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
        struct Voice { uint64_t begin=0; uint32_t address=0; unsigned age=0; uint64_t peakGain=0;
            unsigned part=255,key=255; };
        std::array<Voice,24> voices{};
        unsigned starts=0;
#if defined(SC55_NATIVE_IO_AUDIT)
        mcu_t* cpu=nullptr;
        const sc55::NativeMelodicPlayer* player=nullptr;
        // Diagnostic only: musical identity, not physical slot or exact time.
        using Identity=std::tuple<unsigned,unsigned,uint32_t>;
        std::map<Identity,unsigned> identities;
        std::array<std::array<unsigned,3>,16> partModes{}; // poly, mono, rhythm
        unsigned unknownIdentity=0;
        int tracePart=-1;
        void recordIdentity(unsigned slot,uint32_t sample) {
            if(!cpu && !player) return;
            unsigned part=255,group=255,key=255;
            if(cpu) {
                part=MCU_Read(*cpu,0xa318+slot);
                group=MCU_Read(*cpu,0xa330+slot);
                if(group<24) key=MCU_Read(*cpu,0xa2e8+group);
            } else {
                const auto& allocator=player->allocatorAudit();
                part=allocator.allocations[slot].part;
                group=allocator.allocations[slot].noteGroup;
                if(group<24) key=allocator.noteGroups[group].key;
            }
            if(part<16 && key<128) {
                voices[slot].part=part; voices[slot].key=key;
                ++identities[{part,key,sample}];
                const auto flags=cpu ? MCU_Read(*cpu,0x804d+0x70*part)
                                     : player->partSettings().routing[part].noteFlags;
                ++partModes[part][(flags&0x10) ? 2 : (flags&0x80) ? 0 : 1];
                if(int(part)==tracePart)
                    std::printf("IDENTITY_START %s t=%.6f gs_part=%u key=%u slot=%u sample=%x mode=%x gain=%u,%u\n",
                        name,voices[slot].begin/20000000.0,part,key,slot,sample,flags,
                        pcm->ram2[slot][9],pcm->ram2[slot][10]);
            }
            else ++unknownIdentity;
        }
#endif
        static void sample(void* context,const AudioFrame<int32_t>&) {
            auto& self=*static_cast<Attacks*>(context); const auto& p=*self.pcm;
            const auto active=p.voice_mask&p.voice_mask_pending;
            for(unsigned slot=0;slot<24;++slot) {
                auto& voice=self.voices[slot];
                // PCM's key-on is key && !okey, not an edge in the mask:
                // installing a reused slot clears its latched mode bit 5.
                if((active&(1u<<slot)) && !(p.ram2[slot][7]&0x20)) {
#if defined(SC55_NATIVE_IO_AUDIT)
                    if(int(voice.part)==self.tracePart && voice.age && voice.age<128)
                        std::printf("IDENTITY_RESTART %s t=%.6f gs_part=%u key=%u sample=%x age=%u peak=%llu\n",
                            self.name,voice.begin/20000000.0,voice.part,voice.key,voice.address,voice.age,
                            (unsigned long long)voice.peakGain);
#endif
                    voice={p.cycles-self.base,p.ram1[slot][4],0,0}; ++self.starts;
#if defined(SC55_NATIVE_IO_AUDIT)
                    self.recordIdentity(slot,voice.address);
#endif
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
#if defined(SC55_NATIVE_IO_AUDIT)
                        if(int(voice.part)==self.tracePart)
                            std::printf("IDENTITY_ATTACK %s t=%.6f gs_part=%u key=%u sample=%x peak=%llu\n",
                                self.name,voice.begin/20000000.0,voice.part,voice.key,voice.address,
                                (unsigned long long)voice.peakGain);
#endif
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
#if defined(SC55_NATIVE_IO_AUDIT)
    if(std::getenv("SC55_SONG_IDENTITIES")) {
        h8Attacks.cpu=&cpu;nativeAttacks.player=&player;
        if(const auto* value=std::getenv("SC55_SONG_TRACE_PART")) {
            char* end=nullptr; const auto part=std::strtol(value,&end,10);
            if(end==value || *end || part<0 || part>=16)
                throw std::runtime_error("SC55_SONG_TRACE_PART must be a GS part index 0..15");
            h8Attacks.tracePart=nativeAttacks.tracePart=int(part);
        }
    }
#endif
    if(firstKick) h8Attacks.probeTime=nativeAttacks.probeTime=kickTime-0.0001;
    const bool allKicks=firstKick && std::getenv("SC55_KICK_ALL")!=nullptr;
    h8Attacks.allKicks=nativeAttacks.allKicks=allKicks;
    h8.GetPCM().enable_oversampling=false;
    h8.SetSampleCallback(Attacks::sample,&h8Attacks);
    pcm->output_context=&nativeAttacks; pcm->output_sample=Attacks::sample;
    size_t next=0; unsigned mismatches=0,full=0,notes=0,panelStep=0;
#if defined(SC55_NATIVE_IO_AUDIT)
    std::array<unsigned,16> monoChecks{},monoDifferences{};
    unsigned traceMuteState=~0u;
#endif
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
#if defined(SC55_NATIVE_IO_AUDIT)
            if(nativeAttacks.tracePart>=0 && bytes.size()>=2 && bytes[0]>=0x80 && bytes[0]<0xf0
                && (bytes[0]&15)==player.partSettings().routing[unsigned(nativeAttacks.tracePart)].channel)
                std::printf("IDENTITY_MIDI index=%zu t=%.6f status=%02x data=%u,%u\n",next-1,
                    song.events[next-1].seconds,bytes[0],bytes[1],bytes.size()>2 ? bytes[2] : 0);
#endif
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
#if defined(SC55_NATIVE_IO_AUDIT)
        if(nativeAttacks.tracePart>=0) {
            const auto part=unsigned(nativeAttacks.tracePart);
            const unsigned muted=(!(MCU_Read16(cpu,0x804a+0x70*part)&0x0200) ? 1u : 0u)
                | (player.partMuted(part) ? 2u : 0u);
            if(muted!=traceMuteState) {
                std::printf("IDENTITY_MUTE t=%.6f gs_part=%u H8=%u CPP=%u\n",
                    frame/32000.0,part,muted&1,(muted>>1)&1);
                traceMuteState=muted;
            }
        }
        if(h8Attacks.cpu && !player.queuedEvents()
            && MCU_Read(cpu,0xaaf8)==MCU_Read(cpu,0xaaf9)
            && MCU_Read16(cpu,0xabf8)==MCU_Read16(cpu,0xabf6)) {
            bool settled=true;
            for(unsigned slot=0;slot<24;++slot)
                settled &= MCU_Read(cpu,0xcaf4+slot)==0;
            if(settled) for(unsigned part=0;part<16;++part) {
                const auto key=player.currentMonoKey(part);
                if(!key || (MCU_Read(cpu,0x804d+part*0x70)&0x90)) continue;
                ++monoChecks[part];
                const auto expected=MCU_Read(cpu,0xa070+part);
                if(*key!=expected && ++monoDifferences[part]<=5)
                    std::printf("MONO_KEY_DIFF t=%.6f gs_part=%u H8=%u CPP=%u\n",
                        frame/32000.0,part,expected,*key);
            }
        }
#endif
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
#if defined(SC55_NATIVE_IO_AUDIT)
    if(h8Attacks.cpu) {
        auto counts=h8Attacks.identities;
        for(const auto& [identity,count]:nativeAttacks.identities) counts.try_emplace(identity,0);
        unsigned differences=0;
        for(const auto& [identity,h8Count]:counts) {
            const auto it=nativeAttacks.identities.find(identity);
            const auto nativeCount=it==nativeAttacks.identities.end() ? 0u : it->second;
            if(h8Count==nativeCount) continue;
            ++differences;
            const auto [part,key,sample]=identity;
            std::printf("KEY_IDENTITY_DIFF gs_part=%u key=%u sample=%x H8=%u CPP=%u\n",
                part,key,sample,h8Count,nativeCount);
        }
        std::printf("KEY_IDENTITIES differences=%u unknown=%u/%u (counts are PCM starts, not MIDI note verdicts)\n",
            differences,h8Attacks.unknownIdentity,nativeAttacks.unknownIdentity);
        for(unsigned part=0;part<16;++part) {
            const auto& a=h8Attacks.partModes[part];const auto& b=nativeAttacks.partModes[part];
            if(a[0]+a[1]+a[2]+b[0]+b[1]+b[2])
                std::printf("KEY_PART gs_part=%u poly=%u/%u mono=%u/%u rhythm=%u/%u\n",
                    part,a[0],b[0],a[1],b[1],a[2],b[2]);
            if(monoChecks[part])
                std::printf("MONO_KEY_CHECK gs_part=%u checks=%u differences=%u\n",
                    part,monoChecks[part],monoDifferences[part]);
        }
    }
#endif
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
