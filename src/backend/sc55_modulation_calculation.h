#pragma once
#include "sc55_modulation_work.h"
#include <optional>

namespace sc55
{
// One protected oscillator calculation. The shared PCM source is sampled at
// its latch boundary, not eagerly at entry. Routing and lifetime checks belong
// to the calling voice owner. Tables are immutable inputs, never retained.
class ModulationCalculation
{
public:
    static std::optional<ModulationCalculation> begin(const ModulationBlock& block,uint16_t ticks,
        const std::array<uint16_t,256>& rates,const LfoWaveformTables& tables)
    {
        if(block.waveform>6) return std::nullopt;
        ModulationCalculation result;
        result.block_=block;result.ticks_=ticks;
        result.instructions_=ModulationWork(block,ticks,rates,tables,0,&result.latchInstructions_);
        result.awaitingRandom_=result.latchInstructions_!=0;
        if(!result.awaitingRandom_ && !result.calculate(0,rates,tables,false)) return std::nullopt;
        result.remaining_=(result.awaitingRandom_ ? result.latchInstructions_ : result.instructions_)*12u;
        return result;
    }
    uint32_t remainingCycles() const noexcept {return active_ ? remaining_ : 0;}
    bool needsRandom() const noexcept {return active_ && awaitingRandom_ && remaining_==0;}
    bool complete() const noexcept {return active_ && !awaitingRandom_ && remaining_==0;}
    void advance(uint64_t cycles) noexcept
    {if(active_) remaining_=cycles>=remaining_ ? 0 : remaining_-uint32_t(cycles);}
    bool supplyRandom(uint16_t value,const std::array<uint16_t,256>& rates,const LfoWaveformTables& tables)
    {
        if(!needsRandom()) return false;
        instructions_=ModulationWork(block_,ticks_,rates,tables,value);
        if(instructions_<latchInstructions_ || !calculate(value,rates,tables,true)) return false;
        awaitingRandom_=false;remaining_=(instructions_-latchInstructions_)*12u;
        return true;
    }
    std::optional<ModulationBlock> takeResult() noexcept
    {
        if(!complete()) return std::nullopt;
        active_=false;return block_;
    }
    unsigned referenceInstructions() const noexcept {return active_ && !awaitingRandom_ ? instructions_ : 0;}
private:
    ModulationCalculation()=default;
    bool calculate(uint16_t random,const std::array<uint16_t,256>& rates,const LfoWaveformTables& tables,bool expectedRead)
    {
        struct Input {uint16_t value;unsigned reads=0;uint16_t randomWord() {++reads;return value;}} input{random};
        unsigned registerReads=0;
        const auto read=[&](uint8_t) {++registerReads;return uint8_t(0);};
        return block_.advance(ticks_,rates,tables,read,input)
            && input.reads==unsigned(expectedRead) && registerReads==0;
    }
    ModulationBlock block_;
    uint16_t ticks_=0;
    unsigned instructions_=0,latchInstructions_=0;
    uint32_t remaining_=0;
    bool active_=true,awaitingRandom_=false;
};
}
