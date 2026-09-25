#pragma once

#include <array>
#include <cstdint>

namespace sc55::midiPlayback
{
// Release pedal-retained keys before requesting All Notes Off. The callback
// receives one three-byte MIDI message at a time and owns its delivery policy.
template <typename SendMessage>
void sendAllNotesOff (SendMessage&& sendMessage) noexcept
{
    for (int channel = 0; channel < 16; ++channel)
    {
        const std::array<uint8_t, 3> holdOff {
            static_cast<uint8_t> (0xb0 | channel), 64, 0 };
        const std::array<uint8_t, 3> sostenutoOff {
            static_cast<uint8_t> (0xb0 | channel), 66, 0 };
        sendMessage (holdOff.data(), static_cast<int> (holdOff.size()));
        sendMessage (sostenutoOff.data(), static_cast<int> (sostenutoOff.size()));
    }

    for (int channel = 0; channel < 16; ++channel)
    {
        const std::array<uint8_t, 3> allOff {
            static_cast<uint8_t> (0xb0 | channel), 123, 0 };
        sendMessage (allOff.data(), static_cast<int> (allOff.size()));
    }

    // CC123 only releases keys and can leave a long release envelope audible.
    // Pause/Stop are transport actions, so follow with CC120 to silence voices.
    for (int channel = 0; channel < 16; ++channel)
    {
        const std::array<uint8_t, 3> allSoundOff {
            static_cast<uint8_t> (0xb0 | channel), 120, 0 };
        sendMessage (allSoundOff.data(), static_cast<int> (allSoundOff.size()));
    }
}
}
