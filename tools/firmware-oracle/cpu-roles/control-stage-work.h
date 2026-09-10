#pragma once
#include "mcu.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <limits>
#include <stdexcept>

// Read-only timing of the semantic calculation calls and their interruptible
// gaps. Called at actual instruction entry, never used to drive native time.
struct ControlStageWork
{
    struct Stats {
        uint64_t count=0,wall=0,own=0;
        uint64_t min=std::numeric_limits<uint64_t>::max(),max=0;
        uint64_t ownMin=std::numeric_limits<uint64_t>::max(),ownMax=0;
        void add(uint64_t duration,uint64_t work) {
            ++count; wall+=duration; own+=work;
            min=std::min(min,duration); max=std::max(max,duration);
            ownMin=std::min(ownMin,work); ownMax=std::max(ownMax,work);
        }
    };
    struct Interval { unsigned stage; uint64_t start,own=0; };
    std::array<Stats,5> calculations{},gaps{};
    std::optional<Interval> calculation,gap;
    uint64_t stoppedGaps=0,naturalEnds=0;

    void instruction(mcu_t& cpu,bool inHardwareInterrupt)
    {
        const bool owner=MCU_Read16(cpu,0xfdca)==8 && !inHardwareInterrupt;
        const auto finish=[&](auto& interval,auto& stats) {
            stats[interval->stage].add(cpu.cycles-interval->start,interval->own);
            interval.reset();
        };
        if(owner && cpu.cp==0) {
            constexpr std::array<unsigned,5> calls{0x3303,0x3319,0x332f,0x3345,0x335b};
            constexpr std::array<unsigned,5> returns{0x3306,0x331c,0x3332,0x3348,0x335e};
            if(calculation && (cpu.pc==returns[calculation->stage]
                || (calculation->stage==1 && cpu.pc==0x3393))) {
                naturalEnds+=cpu.pc==0x3393;
                finish(calculation,calculations);
            }
            if(gap && cpu.pc==0x3363) {
                ++stoppedGaps; finish(gap,gaps);
            }
            for(unsigned stage=0;stage<5;++stage) {
                if(cpu.pc==calls[stage]) {
                    if(calculation) throw std::runtime_error("Overlapping control calculation intervals");
                    if(!gap || gap->stage!=stage)
                        throw std::runtime_error("Calculation missed its interruptible entry gap");
                    finish(gap,gaps);
                    calculation=Interval{stage,cpu.cycles};
                }
                // Include ANDC itself; exclude the following calculation's
                // call. The last output operation has no next calculation.
                if(cpu.pc==(stage==0 ? 0x32f0 : returns[stage-1])) {
                    if(gap || calculation) throw std::runtime_error("Overlapping control yield intervals");
                    gap=Interval{stage,cpu.cycles};
                }
            }
        }
        if(owner && !(cpu.cp==0 && cpu.pc<0x752)) {
            if(calculation) calculation->own+=12;
            if(gap) gap->own+=12;
        }
    }
    void report() const
    {
        for(unsigned stage=0;stage<5;++stage) {
            const auto print=[&](const char* kind,const Stats& s) {
                std::printf("[DEBUG-stage-work] %s phase=%u n=%llu wall=%llu own=%llu wallRange=%llu..%llu ownRange=%llu..%llu\n",
                    kind,stage,(unsigned long long)s.count,(unsigned long long)s.wall,
                    (unsigned long long)s.own,(unsigned long long)(s.count?s.min:0),
                    (unsigned long long)s.max,(unsigned long long)(s.count?s.ownMin:0),(unsigned long long)s.ownMax);
            };
            print("calculation",calculations[stage]); print("interruptible",gaps[stage]);
        }
        std::printf("[DEBUG-stage-work] stoppedGaps=%llu naturalEnds=%llu\n",
            (unsigned long long)stoppedGaps,(unsigned long long)naturalEnds);
    }
};
