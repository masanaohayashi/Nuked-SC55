#pragma once
#include "sc55_native_player.h"
#include "sc55_sound_data_import.h"
#include <cmath>
#include <cstdlib>

// Independent MIDI -> audio diagnostic. Both engines use the real PCM chip;
// only the reference runs H8. Metrics are evidence, not a parity assertion.
inline int compareNativeAudio(const char* assetPath,const char* romDirectory,bool dry=false,unsigned program=80,unsigned systemTest=0)
{
    const auto require=[](bool ok,const char* message) {
        if(!ok) throw std::runtime_error(message);
    };
    require(program<128,"Invalid comparison program");
    common::LoadRomsetResult roms;
    require(common::LoadRomset(romDirectory,{},common::RomLoader::Hashing,{},roms)
        ==common::LoadRomsetError{} && roms.picked_name=="mk1-v1.21","Expected v1.21 ROMs");
    std::ifstream input(assetPath,std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    sc55::SoundData data;
    require(data.loadEncoded(bytes),"Cannot load sound data");
    const auto& raw=roms.romset_info.rom_data;
    const auto& r1=raw[static_cast<size_t>(RomLocation::ROM1)];
    const auto& r2=raw[static_cast<size_t>(RomLocation::ROM2)];
    const auto defaults=sc55::ImportSystemDefaults(r1,r2);
    const auto rhythm=sc55::ImportRhythmPresets(r1,r2);
    const auto melodic=sc55::ImportMelodicPresets(r1,r2);
    const auto effects=sc55::ImportEffectsTables(r1,r2);
    auto reference=std::make_unique<Emulator>();
    auto native=std::make_unique<Emulator>();
    for(auto* emu:{reference.get(),native.get()}) {
        require(emu->Init({}) && emu->LoadRoms(roms.romset,roms.romset_info),"Cannot initialize PCM");
        emu->Reset(); emu->GetMCU().native_v121_enabled=false;
        PCM_UseSimulation(emu->GetPCM(),false);
        emu->GetPCM().use_float_effects=false;
        // H8 sets3c=f8 (two DAC frames/pass), native sets3c=0 (one).
        // Compare equal device time, not unequal-duration sample counts.
        emu->GetPCM().enable_oversampling=false;
    }
    auto player=std::make_unique<sc55::NativeMelodicPlayer>(data,native->GetPCM(),
        defaults,rhythm,melodic,effects);
    struct VoiceObservation {
        size_t frame;
        unsigned slot;
        bool active;
        uint32_t address;
        uint16_t phase,pitch;
        std::array<uint16_t,3> commands,levels;
    };
    struct Capture {
        uint64_t frames=0;
        bool recording=false;
        pcm_t* pcm=nullptr;
        bool pinRandom=false;
        const Capture* pitchReference=nullptr;
        size_t pitchCursor=0;
        int pitchLag=0;
        bool measurePitchLag=false;
        std::optional<size_t> firstKeyOn;
        std::vector<AudioFrame<int32_t>> samples;
        std::array<std::optional<VoiceObservation>,2> previous;
        std::vector<VoiceObservation> transitions;
    } h8,cpp;
    h8.pcm=&reference->GetPCM(); cpp.pcm=&native->GetPCM();
    h8.pinRandom=cpp.pinRandom=std::getenv("SC55_COMPARE_PIN_RANDOM")!=nullptr;
    if(h8.pinRandom) std::puts("COMPARISON random pinned in both engines: diagnostic perturbation, not normal audio parity");
    if(std::getenv("SC55_COMPARE_REPLAY_PITCH")) {
        cpp.pitchReference=&h8;
        std::puts("COMPARISON native PCM pitch replaced by H8 trace: diagnostic perturbation only");
    }
    const auto callback=[](void* opaque,const AudioFrame<int32_t>& frame) {
        auto& capture=*static_cast<Capture*>(opaque);
        // Deliberate common-input perturbation, not a fidelity test. It removes
        // the dependence of random reads on H8 instruction/PCM phase.
        if(capture.pinRandom) capture.pcm->ram2[30][10]=0x497b;
        ++capture.frames;
        if(capture.recording) {
            if(!capture.firstKeyOn && (capture.pcm->voice_mask&capture.pcm->voice_mask_pending))
                capture.firstKeyOn=capture.samples.size();
            if(capture.pitchReference && capture.firstKeyOn) {
                const auto& source=*capture.pitchReference;
                const int lag=capture.measurePitchLag && source.firstKeyOn
                    ? int(*source.firstKeyOn)-int(*capture.firstKeyOn) : capture.pitchLag;
                const auto frame=int64_t(capture.samples.size())+lag;
                // Reapply the most recent pitch every PCM pass: the native
                // controller is intentionally overridden by this experiment.
                while(capture.pitchCursor<source.transitions.size()
                    && int64_t(source.transitions[capture.pitchCursor].frame)<=frame)
                    ++capture.pitchCursor;
                std::array<bool,2> found{};
                for(size_t i=capture.pitchCursor;i>0;) {
                    const auto& event=source.transitions[--i];
                    if(!found[event.slot-22] && event.active) {
                        capture.pcm->ram2[event.slot][0]=event.pitch;
                        found[event.slot-22]=true;
                    }
                    if(found[0] && found[1]) break;
                }
            }
            for(unsigned slot=22;slot<24;++slot) {
                const auto& chip=*capture.pcm;
                const auto* ram=chip.ram2[slot];
                VoiceObservation now{capture.samples.size(),slot,
                    bool((chip.voice_mask&chip.voice_mask_pending)&(1u<<slot)),
                    chip.ram1[slot][4],ram[8],ram[0],{ram[3],ram[4],ram[5]},
                    {ram[9],ram[10],ram[11]}};
                auto& previous=capture.previous[slot-22];
                if(!previous || now.active!=previous->active || now.pitch!=previous->pitch
                    || now.commands!=previous->commands) {
                    capture.transitions.push_back(now); previous=now;
                }
            }
            capture.samples.push_back(frame);
        }
    };
    reference->SetSampleCallback(callback,&h8);
    native->SetSampleCallback(callback,&cpp);
    while(reference->GetMCU().cycles<120000000) reference->Step();
    const auto runNative=[&](uint64_t count) {
        const auto end=cpp.frames+count;
        uint64_t steps=0;
        while(cpp.frames<end) {
            player->step();
            require(!player->failed() && ++steps<count*1024+1024,"Native scheduler stalled");
        }
    };
    runNative(16384);
    std::printf("RANDOM_SOURCE h8=%04x native=%04x\n",
        reference->GetPCM().ram2[30][10],native->GetPCM().ram2[30][10]);
    const auto send=[&](std::initializer_list<uint8_t> message) {
        const std::span<const uint8_t> packet(message.begin(),message.size());
        reference->PostMIDI(packet);
        require(player->push(packet)==packet.size(),"Native ingress rejected MIDI");
    };
    if(systemTest==1) {
        const auto settle=[&](uint64_t frames=3200) {
            const auto end=h8.frames+frames;
            while(h8.frames<end) reference->Step();
            runNative(frames);
        };
        unsigned checks=0;
        const auto compare=[&] {
            auto& cpu=reference->GetMCU();
            for(unsigned p=0;p<16;++p) {
                const unsigned base=0x8048+0x70*p;
                const auto& part=player->partSettings().parts[p];
                if(part.bank!=MCU_Read(cpu,base) || part.controls.program!=MCU_Read(cpu,base+1)
                    || player->partSettings().routing[p].noteFlags!=MCU_Read(cpu,base+5)
                    || player->partVoiceCount(p)!=MCU_Read(cpu,0xa1f0+p)) {
                    std::printf("MODE mismatch check=%u part=%u bank=%u/%u program=%u/%u flags=%x/%x voices=%u/%u\n",
                        checks,p,part.bank,MCU_Read(cpu,base),part.controls.program,MCU_Read(cpu,base+1),
                        player->partSettings().routing[p].noteFlags,MCU_Read(cpu,base+5),
                        player->partVoiceCount(p),MCU_Read(cpu,0xa1f0+p));
                    throw std::runtime_error("Native mode state differs from ROM");
                }
            }
            require(player->unsupportedEvents()==0 && native->GetMCU().cycles==0,"Mode test left native path");
            ++checks;
        };
        const auto gs=[&](uint8_t part,uint8_t address,uint8_t value) {
            const auto checksum=uint8_t((128-((0x40+0x10+part+address+value)&127))&127);
            send({0xf0,0x41,0x10,0x42,0x12,0x40,uint8_t(0x10|part),address,value,checksum,0xf7});
            settle(); compare();
        };
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7}); settle(128000); compare();
        send({0xc0,48}); send({0x90,60,100}); settle(); compare();
        gs(1,0x13,1); // Same-value poly write must not stop a sounding note.
        gs(1,0x13,0);
        send({0x90,60,100}); send({0x90,64,100}); settle(); compare();
        gs(1,0x13,0); gs(1,0x13,127);
        for(auto value:{uint8_t(0),uint8_t(1),uint8_t(2),uint8_t(127)}) gs(1,0x14,value);
        send({0xc9,8}); settle(); compare();
        gs(1,0x15,1); gs(2,0x15,1);
        send({0x90,38,100}); settle(); compare();
        gs(1,0x15,2); gs(1,0x15,0); gs(1,0x15,127);
        std::printf("Native GS modes: %u MIDI/ROM state comparisons passed\n",checks);
        return 0;
    }
    if(systemTest==2) {
        const auto settle=[&](uint64_t frames=3200) {
            const auto end=h8.frames+frames;
            while(h8.frames<end) reference->Step();
            runNative(frames);
        };
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7}); settle(128000);
        unsigned checks=0;
        const auto compare=[&] {
            for(unsigned map=0;map<2;++map) for(unsigned i=0;i<0x48c;++i)
                if(player->rhythmRecords()[map][i]!=MCU_Read(reference->GetMCU(),0x8748+map*0x48c+i)) {
                    std::printf("BULK mismatch case=%u map=%u offset=%x native=%x H8=%x\n",checks,map,i,
                        player->rhythmRecords()[map][i],MCU_Read(reference->GetMCU(),0x8748+map*0x48c+i));
                    throw std::runtime_error("Native rhythm bulk differs from ROM");
                }
            ++checks;
        };
        compare();
        for(unsigned block=0;block<32;++block) for(unsigned length:{0u,1u,2u,127u,128u}) {
            if((block&15)==15) continue; //Outside an owned map.
            const auto payloadLength=(block&15)==14 ? std::min(length,24u) : length;
            std::vector<uint8_t> packet{0xf0,0x41,0x10,0x42,0x12,0x49,uint8_t(block),0};
            unsigned sum=0x49+block;
            for(unsigned i=0;i<payloadLength;++i) { const auto value=uint8_t((i*19+block*7)&127); packet.push_back(value); sum+=value; }
            packet.push_back(uint8_t((128-(sum&127))&127)); packet.push_back(0xf7);
            reference->PostMIDI(packet); require(player->push(packet)==packet.size(),"Bulk queue rejected packet");
            settle(); compare();
        }
        require(player->unsupportedEvents()==0 && native->GetMCU().cycles==0,"Bulk test left native path");
        std::printf("Native rhythm bulk: %u complete-map comparisons passed\n",checks);
        return 0;
    }
    bool amplitudeMismatch=false;
    std::optional<int> onsetLag;
    std::array<double,2> dcH8{},dcNative{};
    const auto capture=[&](const char* phase) {
        constexpr size_t count=16384;
        struct Tick {
            size_t frame; uint64_t cycles; uint16_t elapsed;
            std::optional<size_t> firstVoiceFrame;
        };
        std::vector<Tick> ticks;
        h8.samples.clear(); cpp.samples.clear();
        h8.firstKeyOn.reset(); cpp.firstKeyOn.reset();
        h8.previous={}; cpp.previous={};
        h8.transitions.clear(); cpp.transitions.clear();
        cpp.pitchCursor=0;
        cpp.pitchLag=onsetLag.value_or(0);
        cpp.measurePitchLag=std::string_view(phase)=="attack";
        h8.samples.reserve(count+32); cpp.samples.reserve(count+32);
        h8.recording=cpp.recording=true;
        const auto end=h8.frames+count;
        const auto h8Start=reference->GetPCM().cycles, cppStart=native->GetPCM().cycles;
        const auto deadline=reference->GetMCU().cycles+20000000;
        while(h8.frames<end) {
            auto& cpu=reference->GetMCU();
            const bool storingTime=!cpu.sleep && cpu.cp==0 && cpu.pc==0x5af5;
            const bool enteringVoice=!cpu.sleep && cpu.cp==0 && cpu.pc==0x32f0
                && MCU_Read16(cpu,0xfdca)==8;
            reference->Step();
            // Observe a completed store, not an instruction entry that could
            // be interrupted before it executes. Same boundary as ControlClockProbe.
            if(storingTime && cpu.cp==0 && cpu.pc==0x5af9)
                ticks.push_back({h8.samples.size(),cpu.cycles,
                    uint16_t((MCU_Read(cpu,0xac5a)<<8)|MCU_Read(cpu,0xac5b)),{}});
            if(enteringVoice && cpu.cp==0 && cpu.pc>0x32f0 && cpu.pc<0x32f8
                && !ticks.empty() && !ticks.back().firstVoiceFrame)
                ticks.back().firstVoiceFrame=h8.samples.size();
            require(reference->GetMCU().cycles<deadline,"H8 output stalled");
        }
        runNative(count);
        require(reference->GetPCM().cycles-h8Start==native->GetPCM().cycles-cppStart,
            "Audio comparison windows span different device time");
        h8.recording=cpp.recording=false;
        uint64_t minimum=UINT64_MAX,maximum=0,totalElapsed=0;
        for(size_t i=0;i<ticks.size();++i) {
            const auto& tick=ticks[i]; totalElapsed+=tick.elapsed;
            if(i) { const auto delta=tick.cycles-ticks[i-1].cycles;
                minimum=std::min(minimum,delta); maximum=std::max(maximum,delta); }
            if(i<8) std::printf("CONTROL %s H8 frame=%zu elapsed=%u delta_cycles=%llu first_voice_frame=%lld\n",
                phase,tick.frame,tick.elapsed,(unsigned long long)(i?tick.cycles-ticks[i-1].cycles:0),
                tick.firstVoiceFrame ? static_cast<long long>(*tick.firstVoiceFrame) : -1ll);
        }
        require(ticks.size()>1,"H8 control clock was not observed");
        std::printf("CONTROL_SUMMARY %s calls=%zu elapsed=%llu interval_cycles=%llu..%llu nominal=%u\n",
            phase,ticks.size(),(unsigned long long)totalElapsed,(unsigned long long)minimum,
            (unsigned long long)maximum,sc55::ControlTaskClock::kernelTickCycles*sc55::ControlTaskClock::voicePeriodTicks);
        for(const auto* source:{&h8,&cpp}) for(const auto& event:source->transitions)
            std::printf("VOICE %s %s frame=%zu slot=%u active=%u address=%x phase=%x pitch=%x commands=%x,%x,%x levels=%x,%x,%x\n",
                phase,source==&h8?"H8":"CPP",event.frame,event.slot,unsigned(event.active),
                event.address,event.phase,event.pitch,event.commands[0],event.commands[1],event.commands[2],
                event.levels[0],event.levels[1],event.levels[2]);
        double aa=0,bb=0,ee=0; size_t different=0;
        std::array<double,2> sumA{},sumB{};
        for(size_t i=0;i<count;++i) {
            const auto a=h8.samples[i], b=cpp.samples[i];
            sumA[0]+=a.left; sumA[1]+=a.right;
            sumB[0]+=b.left; sumB[1]+=b.right;
            for(const auto pair:{std::pair{a.left,b.left},std::pair{a.right,b.right}}) {
                const double x=pair.first,y=pair.second;
                aa+=x*x; bb+=y*y; ee+=(x-y)*(x-y);
            }
            different+=a.left!=b.left || a.right!=b.right;
        }
        std::printf("AUDIO %s frames=%zu different=%zu h8_rms=%.3f native_rms=%.3f error_rms=%.3f\n",
            phase,count,different,std::sqrt(aa/(2*count)),std::sqrt(bb/(2*count)),std::sqrt(ee/(2*count)));
        const double acA=aa/(2*count)-(sumA[0]*sumA[0]+sumA[1]*sumA[1])/(2.0*count*count);
        const double acB=bb/(2*count)-(sumB[0]*sumB[0]+sumB[1]*sumB[1])/(2.0*count*count);
        std::printf("CENTERED %s h8_mean=%.3f,%.3f native_mean=%.3f,%.3f h8_ac_rms=%.3f native_ac_rms=%.3f\n",
            phase,sumA[0]/count,sumA[1]/count,sumB[0]/count,sumB[1]/count,
            std::sqrt(std::max(0.0,acA)),std::sqrt(std::max(0.0,acB)));
        if(std::string_view(phase)=="held")
            amplitudeMismatch=acA<=0 || std::abs(std::sqrt(std::max(0.0,acB)/acA)-1.0)>0.01;
        if(std::string_view(phase)=="program-settle") {
            for(unsigned c=0;c<2;++c) { dcH8[c]=sumA[c]/count; dcNative[c]=sumB[c]/count; }
        }
        if(std::string_view(phase)=="attack") {
            require(h8.firstKeyOn && cpp.firstKeyOn,"Missing real PCM key-on");
            for(unsigned slot=22;slot<24;++slot) {
                const auto pitchSteps=[&](const Capture& source) {
                    std::vector<std::pair<size_t,uint16_t>> steps;
                    for(const auto& event:source.transitions)
                        if(event.slot==slot && event.active
                            && (steps.empty() || steps.back().second!=event.pitch))
                            steps.emplace_back(event.frame,event.pitch);
                    return steps;
                };
                const auto aSteps=pitchSteps(h8),bSteps=pitchSteps(cpp);
                size_t prefix=0;
                while(prefix<std::min(aSteps.size(),bSteps.size())
                    && aSteps[prefix].second==bSteps[prefix].second) ++prefix;
                std::printf("PITCH_SEQUENCE slot=%u H8_steps=%zu CPP_steps=%zu equal_prefix=%zu\n",
                    slot,aSteps.size(),bSteps.size(),prefix);
                for(size_t i=0;i<std::min({aSteps.size(),bSteps.size(),size_t(12)});++i)
                    std::printf("PITCH_STEP slot=%u step=%zu H8=%04x@%zu CPP=%04x@%zu since_keyon=%zu/%zu\n",
                        slot,i,aSteps[i].second,aSteps[i].first,bSteps[i].second,bSteps[i].first,
                        aSteps[i].first-aSteps.front().first,bSteps[i].first-bSteps.front().first);
                const auto first=[&](const Capture& source) -> const VoiceObservation* {
                    for(const auto& event:source.transitions)
                        if(event.slot==slot && event.active) return &event;
                    return nullptr;
                };
                const auto* a=first(h8); const auto* b=first(cpp);
                require(bool(a)==bool(b),"Different initial voice allocation");
                require(!a || a->address==b->address,"Different initial sample address");
                if(a && (a->pitch!=b->pitch || a->commands!=b->commands)) {
                    std::printf("INITIAL mismatch program=%u slot=%u pitch=%04x/%04x commands=%04x,%04x,%04x/%04x,%04x,%04x\n",
                        program,slot,a->pitch,b->pitch,a->commands[0],a->commands[1],a->commands[2],
                        b->commands[0],b->commands[1],b->commands[2]);
                    throw std::runtime_error("Native MIDI-to-voice initialization differs from H8");
                }
            }
            std::printf("INITIAL program=%u allocation, sample address, pitch and ramp commands match H8\n",program);
            onsetLag=int(*h8.firstKeyOn)-int(*cpp.firstKeyOn);
            std::printf("KEYON h8_frame=%zu native_frame=%zu lag=%d\n",
                *h8.firstKeyOn,*cpp.firstKeyOn,*onsetLag);
        }
        if(onsetLag) {
            // One measured key-on offset for every subsequent phase. Never
            // fit gain, stretch time, or independently align release/tail.
            const int lag=*onsetLag;
            double energy=0,error=0,nativeEnergy=0,product=0;
            size_t aligned=0;
            for(int i=std::max(0,-lag);i<std::min(int(count),int(count)-lag);++i) {
                const auto a=h8.samples[size_t(i+lag)],b=cpp.samples[size_t(i)];
                const std::array<double,2> x{double(a.left)-dcH8[0],double(a.right)-dcH8[1]};
                const std::array<double,2> y{double(b.left)-dcNative[0],double(b.right)-dcNative[1]};
                for(unsigned c=0;c<2;++c) {
                    energy+=x[c]*x[c]; nativeEnergy+=y[c]*y[c];
                    error+=(x[c]-y[c])*(x[c]-y[c]); product+=x[c]*y[c];
                }
                ++aligned;
            }
            require(aligned>0,"Key-on offset exceeds comparison window");
            std::printf("ALIGNED %s frames=%zu lag=%d relative_error=%.6f correlation=%.6f\n",
                phase,aligned,lag,energy>0?std::sqrt(error/energy):0,
                energy>0 && nativeEnergy>0 ? product/std::sqrt(energy*nativeEnergy):0);
        }
    };
    const uint8_t channel=systemTest==3 ? 9 : 0;
    const uint8_t key=systemTest==3 ? 38 : 60;
    if(dry) { send({uint8_t(0xb0|channel),91,0}); send({uint8_t(0xb0|channel),93,0}); }
    std::printf("MODE program=%u %s\n",program,dry?"dry MIDI CC91/93=0":"default effects");
    send({uint8_t(0xc0|channel),uint8_t(program)}); capture("program-settle");
    // Controlled diagnostic only: change one initial condition in the oracle
    // comparison without changing either product engine's initialization.
    if(std::getenv("SC55_COMPARE_SYNC_RANDOM")) {
        native->GetPCM().ram2[30][10]=reference->GetPCM().ram2[30][10];
        std::printf("COMPARISON synchronized pre-note random source=%04x\n",
            native->GetPCM().ram2[30][10]);
    }
    send({uint8_t(0x90|channel),key,100}); capture("attack"); capture("held");
    send({uint8_t(0x80|channel),key,0}); capture("release"); capture("tail");
    require(native->GetMCU().cycles==0,"Native comparison executed H8");
    std::printf("PCM boundary events=%llu\n",static_cast<unsigned long long>(player->pcmBoundaryEvents()));
    std::printf("Held amplitude gate (1%%): %s; no onset alignment or waveform parity is asserted.\n",
        amplitudeMismatch ? "FAIL" : "PASS");
    return amplitudeMismatch ? 1 : 0;
}
