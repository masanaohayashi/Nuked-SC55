#pragma once
#include "sc55_native_player.h"

// Real MIDI -> native owner -> PCM versus H8, comparing semantic groups.
// Physical slots and EG phase are intentionally not equated across clocks.
inline void VerifyReleaseIntegration(Emulator& emu,const RomsetInfo& roms,bool highOnly=false,bool sharedRhythmOnly=false)
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
    bool traceSource=false;
    bool traceRelease=false;
    const auto run=[&](unsigned cycles) {
        const auto end=cpu.cycles+cycles;
        while(cpu.cycles<end) {
            if(traceRelease && cpu.cp==0 && (cpu.pc==0x09f7 || cpu.pc==0x155a || cpu.pc==0x1633
                || cpu.pc==0x0bc1 || cpu.pc==0x0ccd))
                std::printf("RELEASE path=%04x key=%u selector=%u group=%u part=%u retained=%u\n",cpu.pc,
                    cpu.r[1]&255,cpu.r[0]&255,cpu.r[2]&255,cpu.r[3]&255,MCU_Read(cpu,0xa0a0));
            if(traceSource && cpu.cp==0 && (cpu.pc==0x0d4e || cpu.pc==0x0d76 || cpu.pc==0x0dcc))
                std::printf("SOURCE path=%04x r4=%u group=%u latch=%u invalid=%04x\n",cpu.pc,
                    cpu.r[4]&255,cpu.r[2]&255,MCU_Read(cpu,0xa051),MCU_Read16(cpu,0xa1ce));
            emu.Step();
        }
        player.renderFrames(cycles/625);
        if(player.failed()) throw std::runtime_error("Release integration native failure");
    };
    run(120000000);
    unsigned checks=0;
    const auto compare=[&](unsigned cc,unsigned pedal,unsigned part) {
        using Group=std::array<unsigned,5>;
        std::vector<Group> native,h8;
        const auto& a=player.allocatorAudit();
        for(unsigned group=a.partHead[part],n=0;group<24 && n<24;group=a.noteGroups[group].next,++n)
            native.push_back({a.noteGroups[group].key,a.noteGroups[group].status,a.noteGroups[group].retirementFlags,a.noteGroups[group].noteClass,a.noteGroups[group].releaseFlags});
        for(unsigned group=MCU_Read(cpu,0xa210+part),n=0;group<24 && n<24;group=MCU_Read(cpu,0xa240+group),++n)
            h8.push_back({MCU_Read(cpu,0xa2e8+group),MCU_Read(cpu,0xa270+group),
                MCU_Read(cpu,0xa288+group),MCU_Read(cpu,0xa2d0+group),MCU_Read(cpu,0xa300+group)});
        std::sort(native.begin(),native.end());std::sort(h8.begin(),h8.end());
        if(native!=h8 || a.partFlags[part]!=MCU_Read(cpu,0xa230+part)) {
            for(unsigned g=MCU_Read(cpu,0xa210+part),n=0;g<24 && n<24;g=MCU_Read(cpu,0xa240+g),++n)
                std::printf(" H8 group=%u head=%u tail=%u next=%u\n",g,MCU_Read(cpu,0xa2a0+g),MCU_Read(cpu,0xa2b8+g),MCU_Read(cpu,0xa240+g));
            std::printf("Release mismatch CC=%u pedal=%u part=%u flags=%u/%u\n",cc,pedal,part,
                a.partFlags[part],MCU_Read(cpu,0xa230+part));
            for(const auto& g:native) std::printf(" native key=%u status=%u hold=%u type=%u release=%u\n",g[0],g[1],g[2],g[3],g[4]);
            for(const auto& g:h8) std::printf(" H8 key=%u status=%u hold=%u type=%u release=%u\n",g[0],g[1],g[2],g[3],g[4]);
            std::fflush(stdout);throw std::runtime_error("Release command integration differs from H8");
        }
        ++checks;
    };
    const auto send=[&](std::initializer_list<uint8_t> bytes) {
        const auto packet=std::span(bytes.begin(),bytes.size());
        emu.PostMIDI(packet);
        if(player.push(packet)!=packet.size()) throw std::runtime_error("Release MIDI queue full");
        run(2000000);
    };
    if(sharedRhythmOnly) {
        // Two independently routed parts share the actual editable drum map.
        // Compare receiver-local rejection independently of shared map edits.
        const auto mode=[&](uint8_t part,uint8_t map) {
            const unsigned sum=0x40+0x10+part+0x15+map;
            send({0xf0,0x41,0x10,0x42,0x12,0x40,uint8_t(0x10+part),0x15,map,uint8_t(-sum&127),0xf7});
            const auto& settings=player.partSettings();
            if(settings.routing[part].noteFlags!=MCU_Read(cpu,0x804d+part*0x70)
                || settings.parts[part].controls.program!=MCU_Read(cpu,0x8049+part*0x70)
                || uint8_t(settings.parts[part].controls.coarseTuning)!=MCU_Read(cpu,0xab46+part)) {
                std::printf("Rhythm mode part=%u map=%u coarse native=%d H8=%d\n",part,map,
                    settings.parts[part].controls.coarseTuning,int8_t(MCU_Read(cpu,0xab46+part)));
                std::fflush(stdout);throw std::runtime_error("GS mode configuration differs from H8");
            }
            for(unsigned m=0;m<2;++m)
                for(unsigned i=0;i<player.rhythmRecords()[m].size();++i)
                    if(player.rhythmRecords()[m][i]!=MCU_Read(cpu,0x8748+m*0x48c+i))
                        throw std::runtime_error("GS mode changed the shared editable drum map");
        };
        for(uint8_t map:{uint8_t(1),uint8_t(2)}) {
            send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7});run(20000000);
            send({0xb1,101,0});send({0xb1,100,2});send({0xb1,6,76});
            mode(1,map);mode(2,map);
            const uint8_t field=uint8_t((map-1)*16+1);
            const unsigned sum=0x41+field+57+70;
            send({0xf0,0x41,0x10,0x42,0x12,0x41,field,57,70,uint8_t(-sum&127),0xf7});
            mode(1,map); // Assigning the same mode must retain the edited map too.
            send({0xc0,65});
            send({0x90,49,100});compare(65,map,1);
            mode(2,map); // Mode assignment clears rejection even for a rejected shared PC.
            send({0x91,57,100});compare(0x15,map,2);
            send({0xc1,0});
            send({0x90,49,100});compare(0,map,1);
            send({0x91,49,100});compare(0,map,2);
            mode(1,0);mode(1,map);
            send({0x90,49,100});compare(0x15,map,1);
            send({0xc0,0});
            send({0x90,57,100});compare(0,map,1);
        }
        // Existing voices keep their admission identity while the part changes
        // routing. Follow through Note Off/pedals, not just settings readback.
        for(uint8_t pedal:{uint8_t(0),uint8_t(64),uint8_t(66)}) {
            send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7});run(20000000);
            send({0xc0,48});send({0x90,60,100});
            if(pedal) send({0xb0,pedal,127});
            for(uint8_t map:{uint8_t(1),uint8_t(2),uint8_t(0)}) {
                mode(1,map);compare(0x15,pedal,1);
                send({0x90,57,100});compare(0x90,pedal,1);
                send({0x80,57,0});compare(0x80,pedal,1);
            }
            send({0x80,60,0});send({0xb0,64,0});send({0xb0,66,0});
            run(40000000);compare(0x15,pedal,1);
        }
        std::printf("Native shared rhythm: both maps, receiver rejection, shared program and mode changes, %u comparisons PASS\n",checks);
        return;
    }
    if(highOnly) {
        unsigned cases=0;
        const auto* attackOption=std::getenv("SC55_HIGH_ATTACKS");
        const unsigned attacks=attackOption ? unsigned(std::clamp(std::atoi(attackOption),1,4)) : 4;
        for(unsigned key=125;key<128;++key) for(bool present:{false,true}) {
            std::optional<unsigned> program;
            for(unsigned p=0;p<128;++p) {
                const auto* patch=data.patch(*sc55::ResolveV121MelodicPreset(0,p));
                const auto mapping=sc55::MapHighNote(uint8_t(key),patch->common);
                if(bool(!(mapping->tone&0x8000))==present) {program=p;break;}
            }
            if(!program) continue; // Report actual ROM branches exercised.
            send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7});run(20000000);
            send({0xc0,uint8_t(*program)});
            std::printf("High admission program=%u key=%u present=%u\n",*program,key,present);
            for(unsigned attack=0;attack<attacks;++attack) {
                send({0x90,uint8_t(key),100});compare(key,attack,1);
            }
            traceRelease=true;send({0x80,uint8_t(key),0});traceRelease=false;compare(key,4,1);
            ++cases;
        }
        if(!cases) throw std::runtime_error("No high-note branch tested");
        std::printf("Native high admission: %u mapped/absent cases, %u group comparisons PASS\n",cases,checks);
        return;
    }
    for(unsigned cc:{120u,121u,123u,124u,125u,126u,127u}) for(unsigned pedal:{0u,64u,66u}) {
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7});run(20000000);
        send({0xc0,48});
        send({0x90,60,100});send({0x90,60,90});send({0x90,64,80});
        if(pedal) send({0xb0,uint8_t(pedal),127});
        compare(cc,pedal,1);
        send({0xb0,uint8_t(cc),0});compare(cc,pedal,1);
        send({0xb0,64,0});send({0xb0,66,0});
        send({0x80,60,0});send({0x80,60,0});send({0x80,64,0});
        run(40000000);compare(cc,pedal,1);
    }
    for(unsigned mode=0;mode<3;++mode) {
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7});run(20000000);
        send({0xc0,48});
        const unsigned sum=0x40+0x11+0x14+mode;
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0x14,uint8_t(mode),uint8_t(-sum&127),0xf7});
        // More than two attacks forces marking AND actual old-group reclaim.
        // Assignment modes0/1/2 must respectively replace, limit, and layer.
        for(unsigned attack=0;attack<6;++attack) {
            send({0x90,60,uint8_t(80+attack)});compare(0x14,mode,1);
        }
    }
    for(unsigned mode=0;mode<3;++mode) {
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7});run(20000000);
        send({0xc0,48});
        const unsigned sum=0x40+0x11+0x14+mode;
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0x14,uint8_t(mode),uint8_t(-sum&127),0xf7});
        for(unsigned attack=0;attack<4;++attack) {
            // The source key has no group, so CC84 must take fresh admission.
            send({0xb0,84,62});send({0x90,60,uint8_t(80+attack)});compare(84,mode,1);
        }
        send({0xb0,84,60});traceSource=true;send({0x90,64,100});traceSource=false;compare(84,mode,1);
        // The invalidation is now consumed. A following source must reuse
        // normally rather than remaining stuck in the fresh-admission path.
        send({0xb0,84,64});traceSource=true;send({0x90,67,100});traceSource=false;compare(84,mode,1);
    }
    for(uint8_t portamento:{uint8_t(0),uint8_t(127)}) {
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7});run(20000000);
        send({0xc0,48});send({0xb0,126,0});send({0xb0,65,portamento});send({0xb0,84,62});
        for(uint8_t key:{uint8_t(60),uint8_t(64),uint8_t(67)}) {
            send({0x90,key,100});compare(126,portamento,1);
            if(player.currentMonoKey(1)!=MCU_Read(cpu,0xa071))
                throw std::runtime_error("Mono admission key differs from H8");
        }
        for(uint8_t key:{uint8_t(67),uint8_t(64),uint8_t(60)}) {
            send({0x80,key,0});compare(126,portamento,1);
            if(player.currentMonoKey(1)!=MCU_Read(cpu,0xa071))
                throw std::runtime_error("Held-key return differs from H8");
        }
    }
    std::printf("Native release integration: %u group comparisons, stop/pedal/assignment/source/mono-return PASS\n",checks);
}
