#pragma once
#include "mcu.h"
#include "sc55_lfo.h"
#include <optional>
#include <cstdio>
#include <stdexcept>

// Whole second-LFO routing decision. Compare before any detach interrupt
// window or local oscillator update. The H8 owner table is diagnostic input
// only; RouteVoiceModulation itself consumes semantic voice indices/state.
struct ModulationRoutingProbe
{
    std::array<sc55::VoiceModulation,24> voices{};
    std::array<uint8_t,24> sources{};
    std::array<uint16_t,24> bases{};
    bool active=false;
    unsigned channel=0,instructions=0,expectedWork=0;
    enum class Exit { local, shared, detached } exit=Exit::local;
    std::array<uint64_t,3> checks{};

    sc55::VoiceModulation read(mcu_t& cpu,unsigned base) const {
        const auto word=[&](int offset) {return MCU_Read16(cpu,uint16_t(base+offset));};
        sc55::VoiceModulation voice;
        voice.firstStage=word(0);voice.partialIdentity=word(0x9e);
        voice.field99=MCU_Read(cpu,base+0x99);voice.field9b=MCU_Read(cpu,base+0x9b);
        voice.sharing=MCU_Read(cpu,base-81);voice.fieldA2=MCU_Read(cpu,base+0xa2);
        auto& block=voice.block;
        for(unsigned i=0;i<3;++i) {block.depth[i]=word(-94+2*int(i));block.output[i]=word(-88+2*int(i));}
        block.rateIndex=MCU_Read(cpu,base-82);block.rateModifier=int16_t(word(-80));
        block.delayRate=word(-78);block.attackRate=word(-76);block.waveform=uint8_t(word(-74)/2);
        block.wave={word(-72),word(-66),word(-64),word(-62)};
        block.delay=word(-70);block.attack=word(-68);
        return voice;
    }
    unsigned source(mcu_t& cpu,unsigned slot) const {
        const auto pointer=MCU_Read16(cpu,0xc87e + 2*slot);
        if(!pointer) return 24;
        for(unsigned i=0;i<24;++i) if(bases[i]==pointer) return i;
        throw std::runtime_error("Unknown modulation source owner");
    }
    void instruction(mcu_t& cpu,bool interrupt) {
        if(interrupt || cpu.cp || MCU_Read16(cpu,0xfdca)!=8) return;
        if(cpu.pc==0x3a7a) {
            if(active) throw std::runtime_error("Overlapping modulation routing");
            for(unsigned i=0;i<24;++i) bases[i]=MCU_Read16(cpu,0x676a+2*i);
            channel=MCU_Read16(cpu,cpu.r[0]-2);
            if(channel>=24 || bases[channel]!=cpu.r[0]) throw std::runtime_error("Invalid modulation route owner");
            for(unsigned i=0;i<24;++i) {voices[i]=read(cpu,bases[i]);sources[i]=uint8_t(source(cpu,i));}
            exit=Exit::local;expectedWork=2;
            if(voices[channel].sharing) {
                if(sources[channel]>=24) throw std::runtime_error("Missing shared modulation owner");
                const auto& other=voices[sources[channel]];const auto& voice=voices[channel];
                expectedWork=10;
                bool valid=other.firstStage<=12;
                if(valid) {expectedWork+=2;valid=other.field9b==voice.field9b;}
                if(valid) {expectedWork+=2;valid=other.field99==voice.field99;}
                if(valid) {expectedWork+=2;valid=other.partialIdentity==voice.partialIdentity;}
                if(valid) {exit=Exit::shared;expectedWork+=20;}
                else {
                    exit=Exit::detached;expectedWork+=6+3+23*7;
                    for(unsigned i=0;i<24;++i)
                        if(i!=channel && sources[i]==sources[channel]) ++expectedWork;
                }
            }
            const auto result=sc55::RouteVoiceModulation(channel,voices,sources);
            if(result==sc55::ModulationRoute::invalidInput) throw std::runtime_error("Invalid native modulation route");
            if((result==sc55::ModulationRoute::shared)!=(exit==Exit::shared))
                throw std::runtime_error("Modulation routing decision differs");
            active=true;instructions=0;
        }
        if(!active) return;
        const unsigned endpoint=exit==Exit::local ? 0x3b26 : exit==Exit::shared ? 0x3aea : 0x3b11;
        if(cpu.pc==endpoint) {
            for(unsigned i=0;i<24;++i)
                if(sources[i]!=source(cpu,i)) throw std::runtime_error("Modulation follower relinking differs");
            const auto actual=read(cpu,bases[channel]);const auto& predicted=voices[channel];
            const auto& a=actual.block;const auto& b=predicted.block;
            if(actual.sharing!=predicted.sharing || a.depth!=b.depth || a.output!=b.output
                || a.rateIndex!=b.rateIndex || a.rateModifier!=b.rateModifier || a.waveform!=b.waveform
                || a.delayRate!=b.delayRate || a.attackRate!=b.attackRate || a.delay!=b.delay || a.attack!=b.attack
                || a.wave.phase!=b.wave.phase || a.wave.held!=b.wave.held
                || a.wave.smoothed!=b.wave.smoothed || a.wave.output!=b.wave.output)
                throw std::runtime_error("Whole modulation routing state differs");
            if(instructions!=expectedWork) {
                std::fprintf(stderr,"[DEBUG-modulation-routing] route=%u instructions=%u/%u\n",unsigned(exit),instructions,expectedWork);
                throw std::runtime_error("Modulation routing work differs");
            }
            ++checks[unsigned(exit)];active=false;return;
        }
        if(cpu.pc>=0x752) ++instructions;
    }
    void report() const {
        std::printf("[DEBUG-modulation-routing] state/work PASS local=%llu shared=%llu detached=%llu\n",
            (unsigned long long)checks[0],(unsigned long long)checks[1],(unsigned long long)checks[2]);
    }
};
