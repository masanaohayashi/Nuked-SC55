#pragma once
#include <array>
#include <map>
#include <limits>
#include <cstdio>
#include "mcu_timer.h"
#include "sc55_control_clock.h"

// Read-only evidence at the actual AC5A store, not at a PC which can be
// interrupted before executing. Does not alter clock/device/task state.
struct ControlClockProbe
{
    std::array<uint64_t,256> elapsed{};
    std::map<uint16_t,uint64_t> periods;
    uint64_t calls = 0, lastCycles = 0, minimumCycles = UINT64_MAX, maximumCycles = 0;
    uint64_t totalElapsed = 0;
    void observe(mcu_t& mcu)
    {
        const auto word = [&](unsigned at) { return uint16_t((MCU_Read(mcu,at)<<8)|MCU_Read(mcu,at+1)); };
        const auto task = word(0xfdca), ticks = word(0xac5a);
        if (task != 8 || ticks > 255 || MCU_Read(mcu,0xfe08+task) != 0)
            throw std::runtime_error("Unexpected control-task elapsed handoff");
        const auto& timer = *mcu.timer;
        if (timer.frt[1].tcr != 0x22 || timer.frt[1].ocra != 0x138 || (timer.frt[1].tcsr&1) == 0
            || timer.frt_step_table[2] != 31 || word(0xfe24+2*task) != sc55::ControlTaskClock::voicePeriodTicks)
            throw std::runtime_error("Control-task timer configuration changed");
        ++elapsed[ticks]; ++periods[word(0xfe24+2*task)]; totalElapsed += ticks;
        if (calls != 0)
        {
            const auto delta = mcu.cycles-lastCycles;
            minimumCycles = std::min(minimumCycles,delta); maximumCycles = std::max(maximumCycles,delta);
        }
        ++calls; lastCycles = mcu.cycles;
    }
    void report() const
    {
        if (calls == 0) throw std::runtime_error("Control clock probe did not execute");
        std::printf("Control task clock: %llu dispatches, %llu elapsed expirations, interval cycles %llu..%llu\n",
            (unsigned long long)calls,(unsigned long long)totalElapsed,
            (unsigned long long)minimumCycles,(unsigned long long)maximumCycles);
        for (auto [period,count] : periods)
            std::printf("Control task period: %u kernel ticks (%llu dispatches)\n",period,(unsigned long long)count);
        for (unsigned ticks = 0; ticks < elapsed.size(); ++ticks)
            if (elapsed[ticks]) std::printf("Control task elapsed: %u -> %llu dispatches\n",ticks,(unsigned long long)elapsed[ticks]);
    }
};
