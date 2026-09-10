#pragma once
#include "sc55_envelope_work.h"

namespace sc55
{
// Complete periodic filter calculation, using native state/math and typed
// inputs. Reference work is metadata only; there is no CPU/PC/trace input.
struct FilterControlWork
{
    sc55::SecondEnvelopeReleaseState segment;
    sc55::SecondEnvelopePcmState pcm;
    unsigned instructions=0;

    static std::optional<FilterControlWork> evaluate(sc55::SecondEnvelopeReleaseState state,
        sc55::SecondEnvelopePcmState pcm,bool bypass,uint16_t ticks,
        const sc55::SecondEnvelopeTiming& timing,const sc55::EnvelopeTimes& times,
        const sc55::SecondEnvelopeOutputInputs& input,uint8_t base,uint8_t controller,
        uint8_t limit,const sc55::SecondEnvelopePcmTables& tables)
    {
        const auto before=state;
        const auto result=state.advance(bypass,ticks,timing,times,pcm.level);
        using Result=sc55::SecondEnvelopeReleaseState::Result;
        if(result==Result::invalidInput) return std::nullopt;
        if(bypass) return FilterControlWork{state,pcm,3};
        const bool transition=before.progress.position==65535;
        const unsigned stage=transition ? (before.stage<12 ? before.stage+2 : 22) : before.stage;
        unsigned count=8; // Bypass gate, state/progress lookup, stage dispatch.
        if(transition) {
            count+=6;
            if(stage>=4 && stage<=8) count+=7+unsigned(stage<8);
            else if(stage==22) count-=2; // Terminal transition goes directly to sustain output.
            else ++count; // Reload the transitioned stage before dispatch.
        }
        if(result==Result::idle) return FilterControlWork{state,pcm,count+1};
        if(stage==0 || stage==10 || stage==22) count+=5;
        else {
            const bool attack=stage<=4,release=stage==12;
            const unsigned control=attack ? timing.attack : release ? timing.release : timing.decay;
            const unsigned parameter=state.parameter;
            count+=attack ? 4 : 6;
            const bool adjusted=!attack || timing.attackControlEnabled;
            if(attack && adjusted) count+=4;
            if(adjusted) {
                const int value=int(parameter)+2*(int(control)-64);
                count+=control<64 ? 4+2*unsigned(value<0) : 3+unsigned(value>127);
            }
            const auto index=unsigned(std::clamp(int(parameter)+(adjusted ? 2*(int(control)-64) : 0),0,127));
            const auto key=release ? timing.releaseKeyScale : timing.keyScale;
            const auto velocity=attack ? timing.attackVelocityScale : timing.decayReleaseVelocityScale;
            const bool saturatedKey=(uint32_t(times[index])*key>>16)>=255;
            const auto keyTime=sc55::ScaleEnvelopeTime(times[index],key);
            const bool saturatedVelocity=(uint32_t(keyTime)*velocity>>16)>=255;
            const auto duration=sc55::ScaleEnvelopeTime(keyTime,velocity);
            count+=5+(saturatedKey ? 2 : 3)+3+(saturatedVelocity ? 2 : release ? 4 : 5);
            count+=2; // Short-duration gate.
            if(duration<=8) count+=4;
            else {
                count+=14; // Elapsed accumulation and normalized progress.
                const auto increment=uint16_t(524288u/duration);
                const auto elapsed=uint16_t(ticks+before.progress.deferredTicks);
                const auto position=uint32_t(elapsed)*increment+(transition ? 0 : before.progress.position);
                if(position>65535) count+=8;
                else {
                    const auto start=state.start,target=state.target;
                    const bool negativeStart=bool(start&0x8000),negativeTarget=bool(target&0x8000);
                    if(!negativeStart && !negativeTarget) count+=target>=start ? 12 : 13;
                    else if(!negativeStart) count+=12+unsigned(unsigned(start)+uint16_t(0u-target)>32767);
                    else if(!negativeTarget) count+=11+unsigned(unsigned(target)+uint16_t(0u-start)>32767);
                    else count+=uint16_t(0u-target)>=uint16_t(0u-start) ? 14 : 15;
                }
            }
        }
        const auto output=SecondEnvelopeOutputWork::evaluate(state.level,input,pcm.control,
            base,controller,limit,pcm.level,state.stepTime,tables);
        if(!output) return std::nullopt;
        pcm.output=output->output; pcm.level=output->conversion.level;
        pcm.command=output->conversion.command; pcm.control=output->conversion.control;
        return FilterControlWork{state,pcm,count+output->instructions};
    }
};

}
