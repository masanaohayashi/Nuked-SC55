#pragma once
#include "mcu.h"
#include <array>
#include <cstdio>
#include <optional>
#include <stdexcept>

// Partition a complete periodic pass's device time, not sampled host CPU time.
// H8 instruction entry is observed after IRQ dispatch. Suspended IRQ frames
// remain associated with their task, so another runnable task is not counted
// as execution of the interrupt that woke it.
struct ControlPassWork
{
    struct Work {
        std::array<uint64_t,10> tasks{};
        uint64_t kernel=0,irq=0;
        uint64_t total() const {
            uint64_t result=kernel+irq;
            for(auto value:tasks) result+=value;
            return result;
        }
    };
    struct Pending { uint64_t start; unsigned voices; Work work; };
    struct Stats { uint64_t count=0,wall=0,unobserved=0; Work work; };
    std::optional<Pending> pending;
    std::array<Stats,25> byVoices{};

    void instruction(mcu_t& cpu,bool inIrq)
    {
        const auto task=unsigned(MCU_Read16(cpu,0xfdca));
        if(task==8 && !inIrq && cpu.cp==0) {
            if(cpu.pc==0x5af9) {
                if(pending) throw std::runtime_error("Periodic pass restarted without its exit");
                const auto free=unsigned(MCU_Read(cpu,0xa3c1));
                if(free>24) throw std::runtime_error("Invalid periodic pass voice count");
                pending=Pending{cpu.cycles,24-free,{}};
            }
            if(cpu.pc==0x5b70 && pending) {
                auto& stats=byVoices[pending->voices];
                const auto wall=cpu.cycles-pending->start;
                const auto observed=pending->work.total();
                if(observed>wall) throw std::runtime_error("Periodic pass work exceeds its device interval");
                ++stats.count; stats.wall+=wall; stats.unobserved+=wall-observed;
                stats.work.kernel+=pending->work.kernel;stats.work.irq+=pending->work.irq;
                for(unsigned i=0;i<10;++i) stats.work.tasks[i]+=pending->work.tasks[i];
                pending.reset();
            }
        }
        if(!pending) return;
        if(inIrq) pending->work.irq+=12;
        else if(cpu.cp==0 && cpu.pc<0x752) pending->work.kernel+=12;
        else {
            if(task>=10) throw std::runtime_error("Invalid periodic pass instruction owner");
            pending->work.tasks[task]+=12;
        }
    }

    void report() const
    {
        for(unsigned voices=0;voices<byVoices.size();++voices) {
            const auto& s=byVoices[voices];
            if(!s.count) continue;
            std::printf("[DEBUG-pass-partition] voices=%u passes=%llu wall=%llu kernel=%llu irq=%llu unobserved=%llu tasks=",
                voices,(unsigned long long)s.count,(unsigned long long)s.wall,
                (unsigned long long)s.work.kernel,(unsigned long long)s.work.irq,
                (unsigned long long)s.unobserved);
            for(auto value:s.work.tasks) std::printf(" %llu",(unsigned long long)value);
            std::puts("");
        }
    }
};
