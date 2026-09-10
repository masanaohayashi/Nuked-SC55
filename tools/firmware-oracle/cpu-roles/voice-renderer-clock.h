#pragma once
#include "sc55_native_player.h"
#include "sc55_sound_data_import.h"

// Regression: two native controllers, identical ROM data and input,
// different voice math. No H8 execution or private product inspection API.
inline void VerifyVoiceRenderingClock(const RomsetInfo& roms,Romset family,uint8_t program=48,bool measure=false,bool independent=false)
{
    const auto& raw=roms.rom_data;
    const auto& r1=raw[size_t(RomLocation::ROM1)];
    const auto& r2=raw[size_t(RomLocation::ROM2)];
    const auto encoded=sc55::ImportSoundData(r1,r2);
    sc55::SoundData data;
    if(!data.loadEncoded(encoded)) throw std::runtime_error("Data import failed");
    std::array<std::unique_ptr<Emulator>,2> devices;
    std::array<std::unique_ptr<sc55::NativeMelodicPlayer>,2> players;
    std::array<AudioFrame<int32_t>,2> output{};
    auto independentSignal=std::make_unique<sc55::SignalRenderer>();
    auto referenceEffects=std::make_unique<PCMEffects>();
    long double busError=0,busPower=0,dacError=0,dacPower=0;
    uint64_t samples=0;
    std::array<long double,3> stateError{};
    uint64_t voiceSamples=0;
    for(unsigned side=0;side<2;++side) {
        devices[side]=std::make_unique<Emulator>();
        auto& device=*devices[side];
        if(!device.Init({}) || !device.LoadRoms(family,roms)) throw std::runtime_error("Device load failed");
        device.Reset(); device.GetPCM().use_float_effects=false;
        players[side]=std::make_unique<sc55::NativeMelodicPlayer>(data,device.GetPCM(),
            sc55::ImportSystemDefaults(r1,r2),sc55::ImportRhythmPresets(r1,r2),
            sc55::ImportMelodicPresets(r1,r2),sc55::ImportEffectsTables(r1,r2));
        PCM_UseSimulation(device.GetPCM(),independent || side!=0);
        if(independent) {
            auto& pcm=device.GetPCM();
            pcm.use_float_effects=true;
            pcm.effects=side ? &independentSignal->effects : referenceEffects.get();
            pcm.effects_dirty=true;
            if(side) {
                independentSignal->initializeClock(pcm.nfs ? pcm.tv_counter : pcm.ram2[31][8],pcm.ram2[30][10],pcm.cycles/625);
                pcm.native_signal=independentSignal.get();
            }
        }
        if(measure || independent) device.SetSampleCallback([](void* context,const AudioFrame<int32_t>& value) {
            *static_cast<AudioFrame<int32_t>*>(context)=value;
        },&output[side]);
    }
    for(unsigned frame=0;frame<(independent?131072u:measure?65792u:program==80?65536u:131072u);++frame) {
        if(frame==16448) {
#if defined(SC55_NATIVE_IO_AUDIT)
            for(auto& d:devices) {
                auto& pcm=d->GetPCM();
                pcm.auditReads.fill(0); pcm.auditWrites.fill(0);
                pcm.auditVoiceImports=pcm.auditEffectImports=0;
            }
#endif
            const uint8_t midi[]{0xc0,program,0x90,60,100};
            for(auto& player:players) player->push(midi);
        }
        if(measure && frame==49344) {
            const uint8_t off[]{0x80,60,0}; for(auto& player:players) player->push(off);
        }
        if(independent && frame==24576) {
            for(uint8_t note=48;note<72;++note) {
                const uint8_t on[]{0x90,note,100};
                for(auto& player:players) player->push(on);
            }
        }
        if(independent && frame==32768) {
            const uint8_t controls[]{0xb0,91,110,93,100,10,24,0xe0,0,80};
            for(auto& player:players) player->push(controls);
        }
        if(independent && frame==49344) {
            const uint8_t off[]{0xb0,123,0}; for(auto& player:players) player->push(off);
        }
        if(independent && (frame==28672 || frame==40960)) {
            const uint8_t macro=frame==28672?5:7;
            const uint8_t packet[]{0xf0,0x41,0x10,0x42,0x12,0x40,1,0x38,macro,
                uint8_t((0u-0x40-1-0x38-macro)&127),0xf7};
            for(auto& player:players) player->push(packet);
        }
        if(independent) {
            const auto push=[&](std::span<const uint8_t> bytes) { for(auto& p:players) p->push(bytes); };
            if(frame==52000) { const uint8_t bytes[]{0x90,60,100}; push(bytes); }
            if(frame==54000) { const uint8_t bytes[]{0xb0,120,0}; push(bytes); }
            if(frame==56000) { const uint8_t bytes[]{0xb0,126,1,0x90,60,100}; push(bytes); }
            if(frame==58000) { const uint8_t bytes[]{0x90,64,100}; push(bytes); }
            if(frame==60000) { const uint8_t bytes[]{0xc0,1,0x90,67,100}; push(bytes); }
            if(frame==62000) {
                const uint8_t bytes[]{0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7}; push(bytes);
            }
            if(frame==110000) {
                for(auto& p:players)
                    if(p->completedResets()!=1 || p->freeVoices()!=24 || p->queuedEvents()!=0)
                        throw std::runtime_error("Native reset did not drain voices and finish");
                const uint8_t bytes[]{0x90,72,100}; push(bytes);
            }
        }
        if(!measure && !independent && frame==65536) for(auto& device:devices) {
            device->GetPCM().enable_oversampling=true;
            PCM_Write(device->GetPCM(),0x3c,0xf8);
        }
        for(auto& player:players) {
            player->step(); if(player->failed()) throw std::runtime_error("Native controller failed");
        }
        const auto& chip=devices[0]->GetPCM(); const auto& fast=devices[1]->GetPCM();
        if(independent) PCM_SynchronizeNativeReadback(devices[1]->GetPCM());
        if(independent) {
            if(players[0]->freeVoices()!=players[1]->freeVoices()
                || players[0]->completedResets()!=players[1]->completedResets())
                throw std::runtime_error("Native lifecycle scheduling differs between renderers");
            if(fast.irq_assert || players[0]->pcmBoundaryEvents()!=players[1]->pcmBoundaryEvents())
                throw std::runtime_error("Native waveform notifications changed order or used chip IRQ");
            if(output[0].left!=output[1].left || output[0].right!=output[1].right
                || chip.accum_l!=fast.accum_l || chip.accum_r!=fast.accum_r
                || chip.voice_mask!=fast.voice_mask || PCM_HasVoiceBoundary(chip)!=PCM_HasVoiceBoundary(fast)
                || chip.tv_counter!=fast.tv_counter
                || std::memcmp(referenceEffects->delay,independentSignal->effects.delay,sizeof(referenceEffects->delay))) {
                std::printf("Independent signal mismatch frame=%u output=%d/%d expected=%d/%d\n",
                    frame,output[1].left,output[1].right,output[0].left,output[0].right);
                throw std::runtime_error("Independent signal integration diverged");
            }
        }
        if(measure && frame>=16448) {
            for(unsigned channel=0;channel<2;++channel) {
                const long double a=channel?chip.accum_r:chip.accum_l;
                const long double b=channel?fast.accum_r:fast.accum_l;
                const long double c=(channel?output[0].right:output[0].left)/4096.0L;
                const long double d=(channel?output[1].right:output[1].left)/4096.0L;
                busError+=(a-b)*(a-b); busPower+=a*a;
                dacError+=(c-d)*(c-d); dacPower+=c*c; ++samples;
            }
        }
        if(chip.ram2[30][10]!=fast.ram2[30][10])
            throw std::runtime_error("Voice rendering changed the shared PCM random clock");
        const auto active=(chip.voice_mask&chip.voice_mask_pending)&(fast.voice_mask&fast.voice_mask_pending);
        for(unsigned slot=0;slot<24;++slot) if((active>>slot)&1) {
            if(measure && !independent) {
                const std::array<float,3> actual{fast.sim.reference[slot],fast.sim.svf_low[slot],fast.sim.svf_band[slot]};
                constexpr unsigned fields[]{5,3,1};
                for(unsigned stage=0;stage<3;++stage) {
                    const auto raw=chip.ram1[slot][fields[stage]]&0xfffff;
                    const auto expected=raw>=0x80000 ? int32_t(raw)-0x100000 : int32_t(raw);
                    const long double delta=actual[stage]-expected;
                    stateError[stage]+=delta*delta;
                }
                ++voiceSamples;
            }
            for(unsigned e=0;e<3;++e)
                if(chip.ram2[slot][3+e]!=fast.ram2[slot][3+e]
                    || chip.ram2[slot][9+e]!=fast.ram2[slot][9+e])
                    throw std::runtime_error("Voice rendering changed envelope control/readback");
            if(chip.ram2[slot][0]!=fast.ram2[slot][0]
                || chip.ram1[slot][4]!=fast.ram1[slot][4]
                || (chip.ram2[slot][8]&0x3fff)!=(fast.ram2[slot][8]&0x3fff))
                throw std::runtime_error("Voice rendering changed pitch or waveform position");
        }
    }
    if(independent) for(auto& p:players)
        if(p->freeVoices()==24 || p->queuedEvents()!=0)
            throw std::runtime_error("Native synth did not accept a new note after reset");
#if defined(SC55_NATIVE_IO_AUDIT)
    if(independent) {
        const auto& pcm=devices[1]->GetPCM();
        uint64_t reads=0,writes=0;
        for(unsigned a=0;a<64;++a) {
            reads+=pcm.auditReads[a]; writes+=pcm.auditWrites[a];
            if(pcm.auditReads[a] || pcm.auditWrites[a])
                std::printf("Native compatibility address=%02x reads=%llu writes=%llu\n",a,
                    (unsigned long long)pcm.auditReads[a],(unsigned long long)pcm.auditWrites[a]);
        }
        std::printf("Native compatibility totals program=%u reads=%llu writes=%llu\n",program,
            (unsigned long long)reads,(unsigned long long)writes);
        if(reads || writes) throw std::runtime_error("Native lifecycle fell back to byte-register I/O");
        std::printf("Native compatibility imports: voices=%llu effects=%llu\n",
            (unsigned long long)pcm.auditVoiceImports,(unsigned long long)pcm.auditEffectImports);
        if(pcm.auditVoiceImports || pcm.auditEffectImports)
            throw std::runtime_error("Native controls re-imported compatibility rendering state");
    }
#endif
    std::printf("Native voice rendering program=%u: random clock, pitch, wave position and envelope control/readback match\n",program);
    if(measure) std::printf("Renderer stages program=%u: bus relative=%.6f DAC relative=%.6f bus signal RMS=%.3f error RMS=%.3f (20-bit units)\n",
        program,double(std::sqrt(busError/busPower)),double(std::sqrt(dacError/dacPower)),
        double(std::sqrt(busPower/samples)),double(std::sqrt(busError/samples)));
    if(independent) std::printf("Independent signal: exact stereo, mixes, delay memory, gates, waveform events and envelope clock match; events=%llu\n",
        (unsigned long long)players[1]->pcmBoundaryEvents());
    if(measure && !independent) std::printf("Voice state RMS differences: DPCM=%.6f filterLow=%.6f filterBand=%.6f (20-bit units)\n",
        double(std::sqrt(stateError[0]/voiceSamples)),double(std::sqrt(stateError[1]/voiceSamples)),
        double(std::sqrt(stateError[2]/voiceSamples)));
}
