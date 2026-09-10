#pragma once
#include "sc55_voice_lifecycle.h"

namespace sc55
{
// Result and reference work for the existing semantic cutoff conversion. Inputs
// are control values and tables, not CPU registers, a PC or a recorded clock.
// Counts cover473c through its return, in the reference interpreter's units.
// Do not use this partial calculation as a complete voice scheduling budget.
struct EnvelopeConversionWork
{
    uint16_t level,command;
    uint8_t control;
    unsigned instructions;

    static std::optional<EnvelopeConversionWork> evaluate(uint16_t output,uint16_t previous,
        uint8_t control,uint16_t stepTime,const sc55::SecondEnvelopePcmTables& tables)
    {
        if(output>32767 || control>127 || stepTime>8) return std::nullopt;
        const auto originalControl=control;
        const auto level=sc55::ConvertSecondEnvelopePcmLevel(output,control,tables);
        if(!level) return std::nullopt;
        const auto command=sc55::EncodeSecondEnvelopePcmCommand(previous,*level,control,stepTime,tables);
        if(!command) return std::nullopt;

        const unsigned index=output>>8,fraction=output&255;
        const auto difference=uint16_t(tables.levelCurve[index+1]-tables.levelCurve[index]);
        const auto interpolated=uint16_t(tables.levelCurve[index]+((uint32_t(difference)*fraction)>>8));
        const auto ceiling=uint16_t(tables.outputCeiling[control]<<8);
        const auto doubled=uint16_t(interpolated*2u);
        unsigned count=8+8*unsigned(fraction!=0)+4+unsigned(originalControl<8)+6
            +unsigned(doubled>ceiling)+3+unsigned(std::min(doubled,ceiling)>0xe600)+3;
        if(previous==*level) ++count;
        else {
            ++count;
            uint16_t target=*level;
            if(*level<previous) count+=2;
            else {
                count+=4;target&=0xff00;
                if(target==(previous&0xff00)) {
                    target=uint16_t(target+0x100);
                    count+=7+unsigned(target>ceiling);
                    target=std::min(target,ceiling);
                }
            }
            count+=4+unsigned(target>0xe600);
            if(stepTime==0) count+=2;
            else {
                // The normalization loop depends on the actual level delta,
                // not solely on the EG stage or the elapsed tick count.
                static constexpr unsigned limits[]{10,10,9,9,8,8,8,7,7};
                unsigned exponent=limits[stepTime];
                auto magnitude=uint16_t(previous>*level ? previous-*level : *level-previous);
                count+=2;
                for(unsigned shift=0;shift<16;++shift) {
                    const bool normalized=(magnitude&0x8000)!=0;
                    magnitude=uint16_t(magnitude<<1);count+=2;
                    if(normalized) break;
                    ++count;
                    if(exponent--==0) {exponent=0;magnitude>>=1;count+=2;break;}
                }
                const unsigned rate=(((magnitude>>8)/8+1)/2)|sc55::ENVELOPE_EXPONENT[exponent];
                count+=9+(rate==0 ? 1 : 2);
            }
        }
        return EnvelopeConversionWork{*level,*command,control,count+1}; // RTS
    }
};

// Complete second-envelope output path4662..47fa: controller offset, both
// modulation sources, smoothing/ceiling, table interpolation and PCM command.
struct SecondEnvelopeOutputWork
{
    uint16_t output;
    EnvelopeConversionWork conversion;
    unsigned instructions;

    static unsigned modulation(uint16_t level,const sc55::SecondEnvelopeOutputInputs::Source& source)
    {
        const bool firstNegative=bool(source.first&0x8000),secondNegative=bool(source.second&0x8000);
        unsigned count=4;
        bool negative;
        uint16_t magnitude;
        if(firstNegative==secondNegative) {
            negative=firstNegative;
            magnitude=negative ? uint16_t(0u-source.first) : uint16_t(source.first+source.second);
            count+=(negative ? 4 : 3)+2*unsigned(magnitude>6144);
            magnitude=std::min<uint16_t>(6144,magnitude);
        } else {
            const auto sum=uint16_t(source.first+source.second);
            negative=bool(sum&0x8000);magnitude=negative ? uint16_t(0u-sum) : sum;
            count+=2+2*unsigned(negative);
        }
        auto wave=source.waveform;
        count+=2;
        if(wave&0x8000) {count+=negative ? 1 : 2;negative=!negative;wave=uint16_t(0u-wave);}
        const auto amount=uint16_t((uint32_t(magnitude)*uint16_t(wave*2u)+32768u)>>16);
        const auto sum=uint16_t(level+amount);
        count+=6; // Arithmetic/branch and return.
        count+=negative ? unsigned(level<amount)
            : 2*unsigned((~(level^amount)&(level^sum)&0x8000)!=0);
        return count;
    }
    static std::optional<SecondEnvelopeOutputWork> evaluate(uint16_t internal,
        const sc55::SecondEnvelopeOutputInputs& input,uint8_t current,uint8_t base,
        uint8_t controller,uint8_t limit,uint16_t previous,uint16_t stepTime,
        const sc55::SecondEnvelopePcmTables& tables)
    {
        const auto output=sc55::PrepareSecondEnvelopeOutput(internal,input);
        const auto control=sc55::AdvanceSecondEnvelopeControl(current,base,controller,limit,previous,tables);
        if(!output || !control) return std::nullopt;
        const auto conversion=EnvelopeConversionWork::evaluate(*output,previous,*control,stepTime,tables);
        if(!conversion) return std::nullopt;
        unsigned count=7;
        int offsetBase=input.base;
        if(input.control<64) {
            offsetBase+=int(input.control)-64;count+=3+2*unsigned(offsetBase<0);
            offsetBase=std::max(0,offsetBase);
        } else {
            count+=2;
            if(!input.suppressPositiveControl) {
                count+=4+unsigned(input.control>=80);
                offsetBase+=std::min(16,int(input.control)-64);
                count+=unsigned(offsetBase>127);offsetBase=std::min(127,offsetBase);
            }
        }
        const bool negativeInternal=bool(internal&0x8000);
        const int sum=(negativeInternal ? int(internal)-65536 : int(internal))+offsetBase*256;
        count+=5+(negativeInternal ? unsigned(sum<0) : 2*unsigned(sum>32767));
        auto level=uint16_t(std::clamp(sum,0,32767));
        count+=3;
        if(input.offset) {
            ++count;
            if(input.offset&0x8000) {
                const auto magnitude=uint16_t(0u-input.offset);
                count+=3+2*unsigned(level<magnitude);
                level=level<magnitude ? 0 : uint16_t(level-magnitude);
            } else {
                count+=2+unsigned(unsigned(level)+input.offset>32767);
                level=uint16_t(std::min(32767u,unsigned(level)+input.offset));
            }
        }
        for(const auto& source:input.sources) {
            count+=4+modulation(level,source);
            level=sc55::ApplySecondEnvelopeModulation(level,source.first,source.second,source.waveform);
        }
        ++count; // Store post-modulation output.
        const int adjusted=int(base)+2*(64-int(controller));
        count+=6;
        if(controller>=64) count+=3+(adjusted<0 ? 1 : 2+2*unsigned(adjusted>=limit));
        else count+=5+2*unsigned(adjusted>=limit);
        const auto desired=uint8_t(std::clamp(adjusted,0,int(limit)));
        count+=3;
        if(current!=desired) {
            const auto next=uint8_t(current<desired ? current+1 : current-1);
            const unsigned index=std::min(255u,(unsigned(previous)+255u)>>8);
            count+=1+(desired<current ? 2 : 1)+9+unsigned(unsigned(previous)+255>65535)
                +unsigned(tables.smoothingCeiling[index]<next);
        }
        return SecondEnvelopeOutputWork{*output,*conversion,count+conversion->instructions};
    }
};

}
