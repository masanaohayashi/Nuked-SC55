#include "../../Plugins/Source/MidiFifoDispatch.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

static void check (bool condition)
{
    if (! condition)
        std::abort();
}

int main()
{
    // A Note On followed by the Pause All Notes Off sequence. Simulate the
    // UART filling exactly before CC123, then verify the queue cursor stays on
    // its status byte and the complete panic is delivered after room returns.
    const std::array<uint8_t, 10> fifo { 0x90, 60, 100, 0xb0, 123, 0, 0xb0, 120, 0, 0 };
    std::vector<uint8_t> posted;
    const auto blockedRead = sc55::midiPlayback::dispatchMidiFifo (
        fifo, 0, static_cast<uint32_t> (fifo.size() - 1),
        [&] { return posted.size() >= 3; },
        [&] (uint8_t byte) { posted.push_back (byte); });
    check (blockedRead == 3);
    check (posted == std::vector<uint8_t> ({ 0x90, 60, 100 }));

    const auto drainedRead = sc55::midiPlayback::dispatchMidiFifo (
        fifo, blockedRead, static_cast<uint32_t> (fifo.size() - 1),
        [] { return false; }, [&] (uint8_t byte) { posted.push_back (byte); });
    check (drainedRead == fifo.size() - 1);
    check (posted == std::vector<uint8_t> (fifo.begin(), fifo.end() - 1));
    std::cout << "MIDI FIFO preserves Pause All Notes Off through UART backpressure PASS\n";
}
