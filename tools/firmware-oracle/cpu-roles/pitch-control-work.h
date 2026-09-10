#pragma once
#include "sc55_pitch.h"

// A complete periodic pitch operation. Values use the native musical state
// owner; reference work depends only on those same inputs, never CPU registers,
// a program counter, a captured trace, or an average duration. Diagnostic only
// until the remaining control operations and their scheduling are validated.
struct PitchControlWork
{
    sc55::VoicePitchRunner voice;
    unsigned instructions=0;

    static std::optional<PitchControlWork> evaluate(sc55::VoicePitchRunner voice,
        uint16_t ticks,const sc55::PitchModulationInputs& input,uint8_t rateIndex,
        const sc55::PitchGlideRates& rates,uint32_t reference,uint8_t source,
        const sc55::PitchConversion& conversion)
    {
        const auto before=voice;
        const auto result=voice.advance(ticks,false,input,rateIndex,rates,reference,source,conversion);
        if(result==sc55::VoicePitchRunner::Result::invalidInput) return std::nullopt;
        const bool transition=before.envelope.segment.progress.position==65535;
        const auto stage=voice.envelope.stage;
        unsigned count=5; // Progress lookup and stage dispatch.
        if(transition) {
            count+=6;
            if(stage>=4 && stage<=8) {
                const auto& segment=voice.envelope.segment;
                count+=(stage==8 ? 3 : 4)+5+unsigned((segment.start>>16)==(segment.target>>16))
                    +1+(segment.target<segment.start ? 2 : 1)+5;
            }
            else ++count;
        }
        if(result==sc55::VoicePitchRunner::Result::idle)
            return PitchControlWork{voice,count+1};
        if(stage==10 || stage==22) count+=8;
        else {
            const auto elapsed=uint16_t(ticks+before.envelope.segment.progress.deferredTicks);
            const auto position=uint32_t(elapsed)*voice.envelope.segment.increment
                +(transition ? 0 : before.envelope.segment.progress.position);
            count+=22+5*unsigned(position>65535);
        }

        // Offset saturation, two modulation sources, then master/part tuning.
        uint32_t pitch=voice.envelope.output;
        if(input.offset&0x8000) {
            const auto magnitude=uint16_t(0u-input.offset);
            count+=pitch<magnitude ? 13 : 10;
            pitch=pitch<magnitude ? 0 : pitch-magnitude;
        }
        else {
            pitch=(pitch+input.offset)&0xffffff;
            count+=(pitch>>16)==1 ? (pitch>127000 ? 13 : 12) : ((pitch>>16)>1 ? 14 : 11);
            pitch=std::min(pitch,uint32_t(127000));
        }
        count+=8; // Load two source triples and invoke each modulation operation.
        for(const auto& modulation:input.sources) {
            count+=modulationWork(pitch,modulation);
            pitch=sc55::ApplyPitchModulation(pitch,modulation.first,modulation.second,modulation.waveform);
        }
        count+=12; // Tuning input lookup and final stores.
        for(const auto offset:{uint16_t(input.masterTune-1024u),input.partTune}) {
            const bool negative=bool(offset&0x8000);
            const int32_t adjustment=negative ? int32_t(offset)-65536 : int32_t(offset);
            count+=negative ? 5+2*unsigned(bool((pitch+uint32_t(adjustment))&0x800000)) : 3;
            pitch=sc55::AdjustPartPitch(pitch,adjustment);
        }

        const auto increment=before.glide.increment&0xffffff;
        if(increment==0) count+=9;
        else {
            const bool negative=bool(increment&0x800000);
            const auto magnitude=negative ? 0x1000000u-increment : increment;
            const bool crossed=bool((magnitude-uint32_t(ticks)*rates[rateIndex])&0x800000);
            count+=(increment&0xff0000 ? 3 : 5)+6+9;
            count+=negative ? (crossed ? 14 : 15) : (crossed ? 10 : 8);
        }
        const auto delta=(voice.glide.pitch.accumulator-reference-12000u)&0xffffff;
        count+=conversionWork(delta,false)+1; // Final base-rate store.
        count+=10; // Source cache gate and final correction addition/store.
        if(before.glide.pitch.correction.source!=source) {
            const auto correctionDelta=(reference-81000u)&0xffffff;
            count+=conversionWork(correctionDelta,true)+1;
            const int offset=int(source)-128;
            const auto quotient=(uint32_t(offset<0 ? -offset : offset)<<16)
                /conversion.correctionDivisor(reference);
            count+=5+(offset<0 ? 4+unsigned(quotient>=32768) : 2+2*unsigned(quotient>=32768))+1;
        }
        const auto correction=voice.glide.pitch.correction.offset;
        const bool carry=unsigned(correction)+conversion.fromDelta(delta)>65535;
        if(!(correction&0x8000) && carry) count+=2;
        else if((correction&0x8000) && !carry) ++count;
        return PitchControlWork{voice,count+1}; // Return from the complete operation.
    }

private:
    static unsigned modulationWork(uint32_t pitch,const sc55::PitchModulationInputs::Source& source)
    {
        const auto first=source.first,second=source.second;
        bool negative=bool(first&0x8000);
        uint16_t magnitude;
        unsigned count=4;
        if((first&0x8000)==(second&0x8000)) {
            magnitude=negative ? uint16_t(0u-first-second) : uint16_t(first+second);
            count+=negative ? 5 : 3;
            if(magnitude>6000) {magnitude=6000;count+=2;}
        }
        else {
            magnitude=uint16_t(first+second);negative=bool(magnitude&0x8000);count+=2;
            if(negative) {magnitude=uint16_t(0u-magnitude);count+=2;}
        }
        auto waveform=source.waveform;
        count+=2;
        if(waveform&0x8000) {
            waveform=uint16_t(0u-waveform);count+=negative ? 1 : 2;negative=!negative;
        }
        const auto amount=(uint32_t(magnitude)*uint16_t(waveform*2u)+32768u)>>16;
        count+=negative ? 10+2*unsigned(bool((pitch-amount)&0x800000)) : 9;
        return count+3; // Two stores and return.
    }

    static unsigned conversionWork(uint32_t delta,bool correction)
    {
        const bool negative=bool(delta&0x800000);
        const auto magnitude=negative ? 0x1000000u-delta : delta;
        auto octave=magnitude/12000;
        const auto remainder=magnitude%12000;
        unsigned count=5+(negative ? 8 : 4);
        if(!negative && octave!=0) return count+2;
        if(negative && remainder!=0) {++octave;count+=3;}
        count+=15;
        if(negative) {
            count+=2;
            if(octave!=0) {
                count+=(correction ? 3 : 2)+2*octave;
                // The coarse lookup is always >=32768: shifting by fewer
                // than sixteen bits cannot produce a zero divisor.
                if(correction && octave>=16) count+=2;
            }
        }
        return count;
    }
};
