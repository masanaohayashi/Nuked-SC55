#pragma once

#include <cstdint>
#include <span>

namespace sc55
{
// Byte-stream front end for the native controller. No firmware, allocation or
// packet-size limit is needed. SysEx is streamed, so a long GS data-set cannot
// overflow a packet buffer. The receiver must discard an aborted transaction.
class MidiDecoder
{
public:
    enum class Kind { message, realtime, sysexBegin, sysexData, sysexEnd, sysexAbort, receiveRecovery };
    struct Event
    {
        Kind kind;
        uint8_t status = 0, first = 0, second = 0, dataSize = 0;
    };

    void reset() noexcept { running = status = count = expected = 0; inSysEx = false; }

    template <class Sink>
    void push(std::span<const uint8_t> bytes, Sink&& sink)
    {
        for (auto byte : bytes) pushByte(byte, sink);
    }

private:
    template <class Sink>
    void pushByte(uint8_t byte, Sink& sink)
    {
        // Real-time bytes may interrupt absolutely any message, including SysEx.
        if (byte >= 0xf8)
        {
            sink(Event {Kind::realtime, byte});
            return;
        }
        if (inSysEx)
        {
            if (byte < 0x80) { sink(Event {Kind::sysexData, 0, byte}); return; }
            inSysEx = false;
            if (byte == 0xf7) { sink(Event {Kind::sysexEnd, byte}); return; }
            sink(Event {Kind::sysexAbort});
            // The interrupting status starts a new message below.
        }
        if (byte >= 0x80)
        {
            status = byte;
            count = 0;
            expected = 0;
            if (byte < 0xf0)
            {
                running = byte;
                expected = (byte & 0xe0) == 0xc0 ? 1 : 2;
                return;
            }
            running = 0; // All system-common statuses cancel running status.
            switch (byte)
            {
                case 0xf0: inSysEx = true; sink(Event {Kind::sysexBegin, byte}); break;
                case 0xf1: case 0xf3: expected = 1; break;
                case 0xf2: expected = 2; break;
                case 0xf6: sink(Event {Kind::message, byte}); break;
                default: break; // Reserved common statuses / stray EOX.
            }
            return;
        }
        if (expected == 0)
        {
            if (running == 0) return; // Orphan data.
            status = running;
            expected = (status & 0xe0) == 0xc0 ? 1 : 2;
        }
        if (count == 0) first = byte;
        if (++count != expected) return;
        sink(Event {Kind::message, status, first, uint8_t(expected == 2 ? byte : 0), expected});
        count = expected = 0;
    }

    uint8_t running = 0, status = 0, count = 0, expected = 0, first = 0;
    bool inSysEx = false;
};
}
