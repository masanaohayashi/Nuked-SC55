#pragma once

#include <cstdint>
#include <span>

namespace sc55::midiPlayback
{
// Dispatch a circular MIDI byte queue without consuming a message whose
// status byte cannot fit at the emulated UART boundary. Returning the cursor
// leaves that message intact for the next audio render when space is available.
template <typename IsUartNearlyFull, typename PostByte>
uint32_t dispatchMidiFifo (std::span<const uint8_t> fifo, uint32_t read,
                           uint32_t write, IsUartNearlyFull&& isUartNearlyFull,
                           PostByte&& postByte) noexcept
{
    while (read != write)
    {
        const auto byte = fifo[read];
        if (byte >= 0x80 && byte < 0xf7 && isUartNearlyFull())
            break;

        postByte (byte);
        read = (read + 1) % static_cast<uint32_t> (fifo.size());
    }
    return read;
}
}
