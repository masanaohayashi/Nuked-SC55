#pragma once
#include "mcu.h"
#include "amplitude-control-work.h"
#include <map>
#include <cstdio>
#include <stdexcept>

struct AmplitudeControlProbe
{
    std::optional<AmplitudeControlWork> expected;
    sc55::EnvelopeTimes times{};
    bool loaded=false;
    unsigned base=0,instructions=0,entryStage=0;
    uint64_t checks=0,ends=0,delays=0;
    std::map<unsigned,uint64_t> budgets,stages;
    void instruction(mcu_t& cpu,bool inHardwareInterrupt)
    {
        if(MCU_Read16(cpu,0xfdca)!=8 || inHardwareInterrupt) return;
        const auto word=[&](int offset) {return MCU_Read16(cpu,uint16_t(base+offset));};
        const auto byte=[&](int offset) {return MCU_Read(cpu,uint16_t(base+offset));};
        if(cpu.cp==0 && cpu.pc==0x33f4) {
            if(expected) throw std::runtime_error("Overlapping amplitude control");
            if(!loaded) {
                for(unsigned i=0;i<times.size();++i) times[i]=MCU_Read16(cpu,0x6f12+2*i);
                loaded=true;
            }
            base=cpu.r[0];instructions=0;entryStage=word(0);
            if(entryStage>12 || (entryStage&1)) throw std::runtime_error("Invalid active amplitude stage");
            sc55::EnvelopeRunner::Setup setup{};
            for(unsigned i=0;i<5;++i) setup.plan.stages[i]={byte(0x4f+i),byte(-8+int(i))};
            for(unsigned i=0;i<4;++i) setup.targets[i]=byte(0x61+i);
            setup.plan.velocityScale1=word(-30);setup.plan.velocityScale2=word(-28);
            setup.keyScale=word(-34);setup.releaseKeyScale=word(-32);setup.delayIncrement=word(0x10);
            sc55::EnvelopeRunner::State state{{sc55::EnvelopeStage(entryStage/2),{word(8),word(0x12)},
                {byte(0x4f),byte(-8)},byte(0x60),byte(0x61)},word(0x1c),word(0x1e),word(0xe)};
            const auto part=word(0x2e);
            expected=AmplitudeControlWork::evaluate(sc55::EnvelopeRunner(setup,state),MCU_Read16(cpu,0xac5a),
                {MCU_Read(cpu,part+20),MCU_Read(cpu,part+21),MCU_Read(cpu,part+22)},times);
            if(!expected) throw std::runtime_error("Invalid native amplitude inputs");
        }
        if(expected && cpu.cp==0 && (cpu.pc==0x331c || cpu.pc==0x3393 || cpu.pc==0x346b)) {
            using Exit=AmplitudeControlWork::Exit;
            const auto exit=cpu.pc==0x331c ? Exit::updated : cpu.pc==0x3393 ? Exit::finished : Exit::delay;
            const auto& state=expected->state;
            const auto& segment=state.segment;
            if(exit!=expected->exit || expected->firmwareStage!=word(0)
                || segment.progress.position!=word(8) || segment.progress.deferredTicks!=word(0x12)
                || segment.parameter.value!=byte(0x4f) || segment.parameter.flag!=byte(-8)
                || segment.start!=byte(0x60) || segment.target!=byte(0x61)
                || state.level!=word(0x1c) || state.pcmWord!=word(0x1e) || state.delayAccumulator!=word(0xe))
                throw std::runtime_error("Complete native amplitude state differs from H8");
            if(instructions!=expected->instructions) {
                std::fprintf(stderr,"[DEBUG-amplitude-work] stage=%u exit=%u instructions=%u/%u\n",
                    entryStage,unsigned(exit),instructions,expected->instructions);
                throw std::runtime_error("Complete native amplitude work differs from H8");
            }
            ++checks;ends+=exit==Exit::finished;delays+=exit==Exit::delay;
            ++budgets[instructions];++stages[entryStage];expected.reset();
        }
        if(expected && !(cpu.cp==0 && cpu.pc<0x752)) ++instructions;
    }
    void report() const
    {
        std::printf("[DEBUG-amplitude-control] %llu value/work comparisons PASS, budgets=%zu ends=%llu delays=%llu\n",
            (unsigned long long)checks,budgets.size(),(unsigned long long)ends,(unsigned long long)delays);
        for(const auto& [stage,count]:stages)
            std::printf("[DEBUG-amplitude-stage] stage=%u count=%llu\n",stage,(unsigned long long)count);
    }
};
