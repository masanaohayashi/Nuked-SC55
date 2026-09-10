#pragma once
#include "sc55_level.h"
#include <optional>

namespace sc55
{
// Whole output-control operation: level composition, startup ramp, pan and
// effect-send smoothing. Reference instruction work is scheduling metadata,
// not a host benchmark or a product delay. No CPU state or device writes.
struct VoiceOutputWork
{
    sc55::VoiceOutputState output;
    unsigned instructions=0;

    static std::optional<VoiceOutputWork> evaluate(sc55::VoiceOutputState output,
        uint16_t stage,const sc55::LevelInputs& level,const sc55::SpatialInputs& spatial,
        const sc55::PanTable& pan)
    {
        const auto before=output;
        const auto result=output.advance(stage,level,spatial,pan);
        if(result==sc55::VoiceOutputState::Result::invalidInput) return std::nullopt;
        if(result==sc55::VoiceOutputState::Result::stopped) return VoiceOutputWork{output,6};
        unsigned count=5+levelWork(level); // Stage gate and composition call.
        count+=9;
        if(before.tva.ramp!=65535) count+=3+unsigned(uint32_t(before.tva.ramp)+8192>65535);
        const auto difference=std::abs(int(output.tva.level)-int(before.tva.level));
        count+=difference==0 ? 1 : 5+(difference<=16 ? 1 : 3);

        count+=3; // Frozen-pan gate.
        if(before.spatial.pan!=65535) {
            count+=4; // Part pan and optional tone-scale lookup.
            int target=spatial.pan;
            if(spatial.hasToneScale) count+=1+adjustPan(target,spatial.panScale);
            int position=spatial.basePan;
            count+=1+adjustPan(position,target);
            count+=2; // Master-pan lookup and bypass gate.
            if(spatial.masterPan) count+=adjustPan(position,spatial.masterPan);
            count+=2;
            if(position!=before.spatial.pan) count+=(position<before.spatial.pan ? 3 : 2)+8;
        }
        count+=5+unsigned(spatial.hasToneScale)*8; // Send lookup/scaling.
        const auto scaled=[&](uint8_t value,uint8_t scale) {
            return spatial.hasToneScale ? ((unsigned(value)*scale*2+255)>>8)&255 : unsigned(value);
        };
        const unsigned low=before.spatial.effects&255,high=before.spatial.effects>>8;
        const auto lowTarget=scaled(spatial.reverb,spatial.reverbScale);
        const auto highTarget=scaled(spatial.chorus,spatial.chorusScale);
        count+=9; // Current sends, two comparisons, swaps, store and return.
        if(low!=lowTarget) count+=lowTarget<low ? 3 : 2;
        if(high!=highTarget) count+=highTarget<high ? 3 : 2;
        return VoiceOutputWork{output,count};
    }

private:
    // SUB/branch then bounded addition; caller owns input lookup.
    static unsigned adjustPan(int& value,int control)
    {
        const int sum=value+control-64;
        const unsigned count=4+(control<64 ? 2*unsigned(sum<0) : unsigned(sum>127));
        value=std::clamp(sum,0,127);
        return count;
    }

    static unsigned modulationWork(int32_t& level,int16_t first,int16_t second,int16_t depth)
    {
        bool negative=first<0;
        uint16_t magnitude;
        unsigned count=4;
        if((first<0)==(second<0)) {
            magnitude=negative ? uint16_t(-int32_t(first)-second) : uint16_t(int32_t(first)+second);
            count+=negative ? 5 : 3;
            if(magnitude>0x7f00) {magnitude=0x7f00;count+=2;}
        }
        else {
            const auto sum=int32_t(first)+second;negative=sum<0;
            magnitude=uint16_t(negative ? -sum : sum);count+=2+2*unsigned(negative);
        }
        const bool flip=depth<0;
        count+=2;
        if(flip) count+=negative ? 1 : 2;
        const auto product=(uint32_t(magnitude)*unsigned(flip ? -int32_t(depth) : depth))<<1;
        const auto amount=(product>>16)+unsigned(bool(product&65535));
        count+=7+unsigned(negative!=flip && uint16_t(level)<amount);
        sc55::ApplyModulation(level,first,second,depth);
        return count;
    }

    static unsigned levelWork(const sc55::LevelInputs& input)
    {
        unsigned count=17+4;
        uint32_t scaled=((uint32_t(input.expression)*input.velocity*input.master)<<2)>>8&65535;
        if(input.has_tone_scale) {
            scaled=((scaled*input.tone_scale*2)>>8)&65535;
            scaled=(scaled*0x830eu>>15)&65535;count+=9;
        }
        else {scaled=(scaled*0x8208u>>15)&65535;++count;}
        if(!scaled) return count+2; // Silence shortcut and return.
        int32_t level=scaled;
        count+=2;
        if(input.bias) {
            ++count;
            if(input.bias<0) count+=3+2*unsigned(level<-int32_t(input.bias));
            else ++count;
            level=std::max(0,level+input.bias);
        }
        count+=4+modulationWork(level,input.mod1_a,input.mod1_b,input.mod1_depth);
        count+=4+modulationWork(level,input.mod2_a,input.mod2_b,input.mod2_depth);
        const auto squared=(uint32_t(level)*uint32_t(level))>>16;
        count+=4+((squared*0x208u>>16)<255 ? 2 : 1);
        return count+1;
    }
};

}
