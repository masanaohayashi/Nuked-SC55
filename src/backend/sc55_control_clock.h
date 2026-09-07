#pragma once
#include <cstdint>
#include <optional>

namespace sc55
{
// v1.21 FRT2: OCRA=0138, prescaler32, TIMER_Clock's MCU/2 clock.
// These are device cycles, not host samples. The caller chooses the epoch
// relative to PCM startup; this does not emulate kernel execution latency.
class ControlTaskClock
{
public:
    static constexpr uint32_t kernelTickCycles = (0x138+1)*32*2;
    static constexpr uint16_t voicePeriodTicks = 8;

    bool reset(uint16_t periodTicks = voicePeriodTicks) noexcept
    {
        if (periodTicks == 0 || periodTicks > 0x7fff) return false;
        periodCycles_ = uint32_t(periodTicks)*kernelTickCycles;
        phase_ = 0; pending_ = 0; ready_ = false;
        return true;
    }

    void advance(uint64_t cycles) noexcept
    {
        // Divide first to avoid overflow for arbitrarily large block spans.
        const auto remainder = uint32_t(cycles%periodCycles_)+phase_;
        const uint64_t expirations = cycles/periodCycles_+remainder/periodCycles_;
        phase_ = remainder%periodCycles_;
        if (expirations != 0)
        {
            // 03cd increments a byte; bit7 of the event flags is independent
            // and remains set even when 256 expirations wrap pending to zero.
            pending_ = uint8_t(uint64_t(pending_)+expirations);
            ready_ = true;
        }
    }

    std::optional<uint8_t> consume() noexcept
    {
        if (!ready_) return std::nullopt;
        const auto elapsed = pending_;
        pending_ = 0; ready_ = false; // TRAPA B, 02ee..02fe
        return elapsed;
    }
    uint32_t untilNextExpiration() const noexcept { return periodCycles_-phase_; }

private:
    uint32_t periodCycles_ = voicePeriodTicks*kernelTickCycles;
    uint32_t phase_ = 0;
    uint8_t pending_ = 0;
    bool ready_ = false;
};
}
