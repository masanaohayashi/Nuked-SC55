#pragma once
#include "sc55_lfo.h"

// Input-derived instruction work for one complete modulation block. This is
// diagnostic reference metadata; shared-owner selection and device scheduling
// are outside the operation. No instruction stream or captured timing input.
inline unsigned ModulationWork(const sc55::ModulationBlock& block,uint16_t ticks,
    const std::array<uint16_t,256>& rates,const sc55::LfoWaveformTables& tables,uint16_t randomWord)
{
    unsigned count=3;
    bool delayed=false;
    if(block.delay!=65535) {
        const auto sum=uint32_t(block.delayRate)*ticks+block.delay;count+=6;
        if(sum<=65535) {count+=3;delayed=sum!=65535;}
        if(!delayed) ++count;
    }
    if(!delayed) {
        count+=3;
        if(block.attack==65535) count+=6;
        else {
            count+=8+unsigned(uint32_t(block.attackRate)*ticks+block.attack>65535);
            for(auto depth:block.depth) count+=depth&32768 ? 7 : 4;
        }
    }
    auto rate=uint16_t(rates[block.rateIndex]+block.rateModifier);
    count+=13;
    if(rate>0x28f6) {count+=block.rateModifier<0 ? 2 : 1;rate=block.rateModifier<0 ? 0 : 0x28f6;}
    const auto increment=uint32_t(rate)*ticks;
    const auto phase=uint16_t(block.wave.phase+increment);
    switch(block.waveform) {
        case 0: {
            const auto distance=phase<32768 ? 32768-phase : phase-32768;
            const auto index=unsigned(distance)>>8;
            return count+22+unsigned(phase<32768)+2*unsigned(tables.sine[index]>tables.sine[index+1])
                +unsigned(phase>32768);
        }
        case 1: return count+7+unsigned(phase>=32768);
        case 2: return count+6;
        case 3:
            if(phase==32768) return count+7;
            if(phase==16384 || phase==49152) return count+12;
            return count+17+unsigned(phase<32768 ? phase>16384 : phase<49152);
        default: break;
    }
    const auto doubled=increment<<1;
    const bool sample=uint16_t((doubled>>16)+(((doubled&65535)+block.wave.phase)>>16))!=0;
    count+=4*unsigned(sample);
    if(block.waveform==4) return count+10;
    count+=14+(block.waveform==5 ? 2 : 1);
    const auto signedWord=[](uint16_t value) {return value&32768 ? int(value)-65536 : int(value);};
    const int current=signedWord(block.wave.smoothed),target=signedWord(sample ? randomWord : block.wave.held);
    if(current!=target) {
        count+=3; // Direction branch and arithmetic/overflow gate.
        if(current>target) count+=current-80 < -32768 ? 1 : 3;
        else count+=current+80 > 32767 ? 1 : 2+unsigned(current+80>target);
    }
    return count;
}
