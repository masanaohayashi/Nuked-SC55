#pragma once
#include "sc55_envelope_runner.h"

namespace sc55
{
// A semantic amplitude operation plus input-derived reference work. Completion
// returns a lifecycle decision; allocation/PCM publication remain their owners.
// The delay boundary excludes its interrupt-unmask/outer-return tail.
struct AmplitudeControlWork
{
    sc55::EnvelopeRunner::State state;
    enum class Exit { updated, delay, finished } exit=Exit::updated;
    unsigned instructions=0,firmwareStage=0;

    static std::optional<AmplitudeControlWork> evaluate(sc55::EnvelopeRunner voice,
        uint16_t ticks,sc55::EnvelopeRunner::Controls controls,const sc55::EnvelopeTimes& times)
    {
        using Stage=sc55::EnvelopeStage;
        const auto before=voice.state();
        const auto& setup=voice.setup();
        if(before.segment.stage==Stage::finished) return std::nullopt;
        if(!voice.tick(ticks,controls,times)) return std::nullopt;
        const auto after=voice.state();
        const bool transition=before.segment.progress.position==65535;
        const auto segment=sc55::AdvanceEnvelopeStage(before.segment,setup.plan.stages,setup.targets);
        unsigned count=transition ? 10 : 6;
        if(transition && segment.stage!=Stage::finished) {
            const auto stage=unsigned(segment.stage);
            count+=stage>=2 && stage<=4 ? (stage==4 ? 11 : 12) : 3;
        }
        unsigned firmwareStage=segment.stage==Stage::finished ? 22 : unsigned(segment.stage)*2;
        const auto finish=[&](Exit exit,unsigned extra) {
            return AmplitudeControlWork{after,exit,count+extra,firmwareStage};
        };
        if(segment.stage==Stage::finished) return finish(Exit::finished,2);
        if(segment.stage==Stage::delay) {
            const bool expired=uint32_t(before.delayAccumulator)+setup.delayIncrement>65535;
            if(expired) firmwareStage=2;
            return finish(Exit::delay,expired ? 10 : 7);
        }
        if(segment.stage==Stage::sustain)
            return finish(after.segment.stage==Stage::finished ? Exit::finished : Exit::updated,
                after.segment.stage==Stage::finished ? 9 : 8);
        if(segment.stage==Stage::release && before.level==0) return finish(Exit::finished,4);
        const bool attack=segment.stage==Stage::attack1 || segment.stage==Stage::attack2;
        const bool release=segment.stage==Stage::release;
        const auto control=attack ? controls.attack : release ? controls.release : controls.decay;
        const int adjusted=int(segment.parameter.value)+2*(int(control)-64);
        count+=release ? 8 : 6;
        count+=control<64 ? 4+2*unsigned(adjusted<0) : 3+unsigned(adjusted>127);
        const auto index=unsigned(std::clamp(adjusted,0,127));
        const auto key=release ? setup.releaseKeyScale : setup.keyScale;
        const auto velocity=attack ? setup.plan.velocityScale1 : setup.plan.velocityScale2;
        const auto keyTime=sc55::ScaleEnvelopeTime(times[index],key);
        const auto duration=sc55::ScaleEnvelopeTime(keyTime,velocity);
        count+=5+((uint32_t(times[index])*key>>16)>=255 ? 2 : 3)+3
            +((uint32_t(keyTime)*velocity>>16)>=255 ? 2 : release ? 4 : 5);
        if(duration==0) return finish(Exit::updated,2+7+1);
        unsigned shift=7;
        bool continuation=false;
        if(duration<=8) {
            constexpr unsigned shifts[]{10,10,9,9,8,8,8,7,7};
            shift=shifts[duration];continuation=true;count+=4+8;
        }
        else {
            const auto elapsed=uint16_t(ticks+segment.progress.deferredTicks);
            const auto position=uint32_t(elapsed)*(524288u/duration)+segment.progress.position;
            count+=19+5*unsigned(position>65535);
            if(after.segment.progress.position==65535) count+=4;
            else {
                const bool falling=segment.target<segment.start;
                count+=segment.parameter.flag ? (falling ? 30 : 29) : (falling ? 16 : 14);
            }
        }
        return finish(Exit::updated,encodingWork(before.level,after.level,shift,continuation));
    }

private:
    static unsigned encodingWork(uint16_t previous,uint16_t target,unsigned shift,bool continuation)
    {
        unsigned count=continuation ? 3 : 6;
        if(target==previous) return count+2; // Hold store and return.
        ++count;
        uint16_t magnitude;
        if(target<previous) {magnitude=uint16_t(previous-target);count+=2;}
        else {
            magnitude=uint16_t(target-previous);count+=4;
            if((target&0xff00)==(previous&0xff00)) {
                count+=2+unsigned((target&0xff00)==0xff00);
            }
        }
        const auto encoded=sc55::EncodeRate(magnitude,int(shift));
        for(;;) {
            const bool high=bool(magnitude&0x8000);
            magnitude=uint16_t(magnitude<<1);count+=2;
            if(high) break;
            ++count;
            if(magnitude && shift--!=0) continue;
            count+=2;break;
        }
        return count+9+unsigned(encoded!=0)+2;
    }
};

}
