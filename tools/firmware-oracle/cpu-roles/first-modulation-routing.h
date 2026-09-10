#pragma once
#include "mcu.h"
#include "sc55_lfo.h"
#include <cstdio>
#include <stdexcept>

// Whole first-LFO routing and paired transfer through real task8 execution.
// No firmware writes or product timing changes. Unlike second-LFO sharing,
// destination depths are recomputed while local rate/timing remain owned here.
struct FirstModulationRoutingProbe
{
    explicit FirstModulationRoutingProbe(bool initialization=false) : initializationOnly(initialization)
    { for(auto& voice:voices) voice.firstStage=22; }
    bool initializationOnly=false;
    std::array<sc55::FirstModulationVoice,sc55::voiceCapacity> voices{};
    std::array<uint16_t,24> bases{};
    std::array<uint16_t,128> depths{};
    bool active=false,loaded=false;
    unsigned channel=0,endpoint=0,kind=0;
    std::array<uint64_t,5> checks{}; // Local, shared, detached, paired, initialization copy.
    uint64_t persistentSources=0;

    unsigned source(mcu_t& cpu,unsigned slot) const {
        const auto address=MCU_Read16(cpu,0xc84e + 2*slot);
        if(!address) return sc55::voiceCapacity;
        for(unsigned i=0;i<24;++i) if(bases[i]==address) return i;
        throw std::runtime_error("Unknown first modulation source");
    }
    sc55::FirstModulationVoice read(mcu_t& cpu,unsigned slot) const {
        const auto base=bases[slot];
        const auto word=[&](int offset) {return MCU_Read16(cpu,uint16_t(base+offset));};
        sc55::FirstModulationVoice voice;
        voice.firstStage=word(0);voice.commonIdentity=word(0x9c);
        voice.commonBank=MCU_Read(cpu,base+0x98);voice.field9b=MCU_Read(cpu,base+0x9b);
        voice.sharing={uint8_t(source(cpu,slot)),MCU_Read(cpu,base-25),MCU_Read(cpu,base-115)};
        auto& block=voice.block;
        for(unsigned i=0;i<3;++i) {block.depth[i]=word(-128+2*int(i));block.output[i]=word(-122+2*int(i));}
        block.rateIndex=MCU_Read(cpu,base-116);block.rateModifier=int16_t(word(-114));
        block.delayRate=word(-112);block.attackRate=word(-110);block.waveform=uint8_t(word(-108)/2);
        block.wave={word(-106),word(-100),word(-98),word(-96)};
        block.delay=word(-104);block.attack=word(-102);
        return voice;
    }
    void instruction(mcu_t& cpu,bool interrupt) {
        if(interrupt || cpu.cp || MCU_Read16(cpu,0xfdca)!=(initializationOnly ? 2 : 8)) return;
        if(!active && (initializationOnly ? cpu.pc==0x3d1a : (cpu.pc==0x3985 || cpu.pc==0x3d44))) {
            if(!loaded) {
                for(unsigned i=0;i<128;++i) depths[i]=MCU_Read16(cpu,0x7312+2*i);
                loaded=true;
            }
            for(unsigned i=0;i<24;++i) bases[i]=MCU_Read16(cpu,0x676a+2*i);
            channel=MCU_Read16(cpu,cpu.r[0]-2);
            if(channel>=24 || bases[channel]!=cpu.r[0]) throw std::runtime_error("Invalid first LFO owner");
            for(unsigned i=0;i<24;++i) voices[i]=read(cpu,i);
            const auto pitchDepth=uint8_t(MCU_Read16(cpu,cpu.r[0]+0xa8));
            const auto part=MCU_Read16(cpu,cpu.r[0]+0x2e);
            const auto control=MCU_Read(cpu,part+17);
            const unsigned returnPc=MCU_Read16(cpu,cpu.r[7]);
            if(cpu.pc==0x3d44 || cpu.pc==0x3d1a) {
                unsigned origin=24;
                for(unsigned i=0;i<24;++i) if(bases[i]==cpu.r[2]) origin=i;
                if(origin>=24) throw std::runtime_error("Invalid first modulation copy source");
                const bool copied=initializationOnly
                    ? sc55::InitializeSharedFirstModulation(voices[channel].block,voices[channel].sharing,
                        voices[origin].block,voices[origin].sharing,pitchDepth,control,depths)
                    : sc55::UpdatePairedFirstModulation(voices[channel],voices[origin],pitchDepth,control,depths);
                if(!copied)
                    throw std::runtime_error("Invalid paired first modulation inputs");
                kind=initializationOnly ? 4 : 3;endpoint=returnPc;
            }
            else {
                const bool wasShared=voices[channel].sharing.sharing!=0;
                sc55::FirstVoiceModulationUpdate task;
                const auto result=task.begin(channel,voices,pitchDepth,control,depths);
                if(result==sc55::FirstVoiceModulationUpdate::Result::invalidInput)
                    throw std::runtime_error("Invalid first modulation routing inputs");
                const bool shared=result==sc55::FirstVoiceModulationUpdate::Result::shared;
                kind=shared ? 1 : wasShared ? 2 : 0;
                endpoint=shared ? returnPc : wasShared ? 0x39e1 : 0x39f6;
            }
            active=true;
        }
        if(!active || cpu.pc!=endpoint) return;
        for(unsigned i=0;i<24;++i)
            if(source(cpu,i)!=voices[i].sharing.source) throw std::runtime_error("First LFO follower relinking differs");
        const auto actual=read(cpu,channel);const auto& predicted=voices[channel];
        const auto& a=actual.block;const auto& b=predicted.block;
        if(actual.sharing.sharing!=predicted.sharing.sharing || actual.sharing.baseRate!=predicted.sharing.baseRate
            || a.depth!=b.depth || a.output!=b.output || a.rateIndex!=b.rateIndex || a.rateModifier!=b.rateModifier
            || a.waveform!=b.waveform || a.delayRate!=b.delayRate || a.attackRate!=b.attackRate
            || a.delay!=b.delay || a.attack!=b.attack || a.wave.phase!=b.wave.phase || a.wave.held!=b.wave.held
            || a.wave.smoothed!=b.wave.smoothed || a.wave.output!=b.wave.output)
            throw std::runtime_error("Whole first LFO routing state differs");
        ++checks[kind];
        if(initializationOnly && actual.sharing.sharing) ++persistentSources;
        active=false;
    }
    void report() const {
        if(initializationOnly) {
            std::printf("[DEBUG-first-modulation-initialization] state PASS copies=%llu persistent=%llu\n",
                (unsigned long long)checks[4],(unsigned long long)persistentSources);
            return;
        }
        std::printf("[DEBUG-first-modulation-routing] state PASS local=%llu shared=%llu detached=%llu paired=%llu\n",
            (unsigned long long)checks[0],(unsigned long long)checks[1],(unsigned long long)checks[2],(unsigned long long)checks[3]);
    }
};
