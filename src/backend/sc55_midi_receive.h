#pragma once
#include <cstdint>

namespace sc55
{
// v1.21 UART ingress policy (00:05b3..05e8) and watchdog observation
// (00:06e0..073a). This is receiver state, not MIDI message decoding.
// A watchdog poll is supplied by the device scheduler; it is NOT a timeout
// measured from the last complete MIDI message.
class MidiReceiveState
{
public:
    bool receiveByte(uint8_t byte) noexcept
    {
        activity_ = true; // Even ignored or incomplete messages count.
        if (byte < 0x80) return !discardData_;
        if (byte <= 0xf0) { discardData_ = false; return true; }
        if (byte == 0xf7) return true; // Does not clear the data filter.
        if (byte == 0xfe) armed_ = true;
        else if (byte < 0xf8) discardData_ = true;
        return false;
    }

    // True requests the firmware's communication-error recovery. Arming is
    // one-shot; another FE is required after a timeout. An unarmed poll does
    // not clear activity (06e4 branches directly to 073a).
    bool poll() noexcept
    {
        if (!armed_) return false;
        const bool timeout = !activity_;
        activity_ = false;
        if (timeout) armed_ = false;
        return timeout;
    }
    uint8_t flags() const noexcept
    { return uint8_t((activity_ ? 1 : 0) | (armed_ ? 2 : 0) | (discardData_ ? 16 : 0)); }

private:
    bool activity_ = false, armed_ = false, discardData_ = false;
};

// FRT1 and AC29, with the currently selected UART transmitter as the gate.
// There is no UART register mirror: callers supply its two observable states.
class MidiReceiveTimer
{
public:
    static constexpr uint32_t periodCycles = (0x927c+1)*32*2;
    explicit MidiReceiveTimer(uint8_t divider=1) noexcept : divider_(divider) {}
    bool tick(MidiReceiveState& receiver,bool outputEmpty,bool outputReady) noexcept
    {
        if (!outputEmpty) return false;
        ++divider_;
        if ((divider_&1) || !outputReady) return false;
        if (!(divider_&4)) return false;
        if (receiver.poll()) return true; // Timeout exits before AC29 clear.
        divider_=0;
        return false;
    }
    uint8_t divider() const noexcept { return divider_; }
private:
    uint8_t divider_;
};
}
