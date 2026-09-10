#pragma once
#include "sc55_synth_state.h"
#include <atomic>
#include <array>

// One producer (audio owner, or initialization while audio is stopped), one
// consumer (message thread). Each owns a separate buffer; the third is handed
// over atomically. Slow/no editor drops intermediate snapshots, never audio.
class NativeSynthStateExchange
{
public:
    void publish(const sc55::SynthState& state) noexcept
    {
        states[back]=state;
        back=middle.exchange(back|dirty,std::memory_order_acq_rel)&indexMask;
    }
    sc55::SynthState read() noexcept
    {
        if(middle.load(std::memory_order_acquire)&dirty)
            front=middle.exchange(front,std::memory_order_acq_rel)&indexMask;
        return states[front];
    }
private:
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    static constexpr unsigned dirty=4,indexMask=3;
    std::array<sc55::SynthState,3> states{};
    std::atomic<unsigned> middle{1};
    unsigned back=0,front=2;
};
