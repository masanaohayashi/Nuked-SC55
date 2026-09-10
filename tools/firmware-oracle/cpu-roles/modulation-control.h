#pragma once
#include "mcu.h"
#include "sc55_lfo.h"
#include "modulation-work.h"
#include <map>
#include <optional>
#include <cstdio>
#include <stdexcept>

// Real MIDI-driven whole modulation-block comparison. Random input is sampled
// from the actual H8 PCM read, not inferred from its final output or invented.
struct ModulationControlProbe
{
    std::optional<sc55::ModulationBlock> before;
    std::optional<uint16_t> random;
    std::array<uint16_t,256> rates{};
    sc55::LfoWaveformTables tables;
    bool loaded=false;
    unsigned base=0,returnPc=0,ticks=0;
    unsigned instructions=0;
    std::map<unsigned,uint64_t> budgets;
    uint64_t checks=0,randomChecks=0;
    std::array<uint64_t,7> shapes{};

    void instruction(mcu_t& cpu,bool inHardwareInterrupt)
    {
        if(MCU_Read16(cpu,0xfdca)!=8 || inHardwareInterrupt || cpu.cp) return;
        const auto word=[&](unsigned offset) {return MCU_Read16(cpu,base+offset);};
        if(cpu.pc==0x3b2c) {
            if(before) throw std::runtime_error("Overlapping modulation block");
            if(!loaded) {
                for(unsigned i=0;i<rates.size();++i) rates[i]=MCU_Read16(cpu,0x7012+2*i);
                loaded=true;
            }
            base=cpu.r[1];returnPc=MCU_Read16(cpu,cpu.r[7]);ticks=MCU_Read16(cpu,0xac5a);
            sc55::ModulationBlock block;
            for(unsigned i=0;i<3;++i) {block.depth[i]=word(i*2);block.output[i]=word(6+i*2);}
            block.rateIndex=MCU_Read(cpu,base+12);block.rateModifier=int16_t(word(14));
            block.delayRate=word(16);block.attackRate=word(18);
            if(word(20)>12 || (word(20)&1)) throw std::runtime_error("Invalid LFO shape");
            block.waveform=uint8_t(word(20)/2);block.delay=word(24);block.attack=word(26);
            block.wave={word(22),word(28),word(30),word(32)};
            before=block;random.reset();instructions=0;
        }
        if(!before) return;
        if(cpu.pc==0x3cc3 || cpu.pc==0x3ced) {
            if(random) throw std::runtime_error("Multiple random inputs in LFO update");
            random=cpu.r[4];
        }
        if(cpu.pc!=returnPc) {if(cpu.pc>=0x752) ++instructions;return;}
        auto expected=*before;
        struct RandomInput {
            std::optional<uint16_t> value;
            unsigned reads=0;
            uint16_t randomWord() {
                if(!value || reads++) throw std::runtime_error("Native LFO random-read decision differs");
                return *value;
            }
        } input{random};
        auto unused=[](uint8_t)->uint8_t {throw std::runtime_error("Unexpected LFO register access");};
        if(!expected.advance(uint16_t(ticks),rates,tables,unused,input))
            throw std::runtime_error("Invalid native LFO inputs");
        if(input.reads!=unsigned(random.has_value())) throw std::runtime_error("Missing native LFO random read");
        for(unsigned i=0;i<3;++i)
            if(expected.output[i]!=word(6+i*2)) throw std::runtime_error("Native LFO depth differs");
        if(expected.delay!=word(24) || expected.attack!=word(26) || expected.wave.phase!=word(22)
            || expected.wave.held!=word(28) || expected.wave.smoothed!=word(30) || expected.wave.output!=word(32))
            throw std::runtime_error("Native LFO state differs");
        const auto work=ModulationWork(*before,uint16_t(ticks),rates,tables,random.value_or(0));
        if(work!=instructions) {
            std::fprintf(stderr,"[DEBUG-modulation-work] shape=%u instructions=%u/%u\n",before->waveform,instructions,work);
            throw std::runtime_error("Native LFO work differs");
        }
        ++budgets[instructions];
        ++checks;randomChecks+=input.reads;++shapes[expected.waveform];before.reset();
    }
    void report() const {
        std::printf("[DEBUG-modulation-control] %llu whole-block state/work comparisons PASS, random=%llu budgets=%zu\n",
            (unsigned long long)checks,(unsigned long long)randomChecks,budgets.size());
        for(unsigned i=0;i<shapes.size();++i)
            if(shapes[i]) std::printf("[DEBUG-modulation-shape] shape=%u count=%llu\n",i,(unsigned long long)shapes[i]);
    }
};
