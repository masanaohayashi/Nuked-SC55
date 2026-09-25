#pragma once

#include "sc55_synth.h"
#include "../../Plugins/Source/MidiPlaybackCommands.h"
#include <cstdio>
#include <stdexcept>

inline int probePauseRelease (const auto& rom, const auto& encoded)
{
    const auto& rom1 = rom[size_t (RomLocation::ROM1)];
    const auto& rom2 = rom[size_t (RomLocation::ROM2)];
    sc55::NativeSynth synth (encoded, rom1, rom2,
        rom[size_t (RomLocation::WAVEROM1)], rom[size_t (RomLocation::WAVEROM2)],
        rom[size_t (RomLocation::WAVEROM3)],
        sc55::NativeSynth::VoiceRendering::referenceChip, 24);

    std::array<AudioFrame<int32_t>, 128> audio {};
    auto render = [&] (uint64_t frameCount)
    {
        while (frameCount > 0)
        {
            const auto count = size_t (std::min<uint64_t> (audio.size(), frameCount));
            synth.render (std::span (audio.data(), count));
            frameCount -= count;
            if (synth.failed())
                throw std::runtime_error ("Native engine failed during Pause regression");
        }
    };
    auto send = [&] (std::initializer_list<uint8_t> bytes)
    {
        if (synth.push (std::span (bytes.begin(), bytes.size())) != bytes.size())
            throw std::runtime_error ("Native engine rejected Pause regression MIDI");
    };

    send ({ 0xc0, 0 });       // Acoustic Grand Piano.
    send ({ 0xb0, 64, 127 });  // Hold pedal.
    send ({ 0x90, 60, 100 });  // C4 on.
    render (32000);
    send ({ 0x80, 60, 64 });   // Key released; the pedal keeps it sounding.
    render (32000);

    if (synth.state().parts[1].voices == 0)
        throw std::runtime_error ("Pause regression did not establish a pedal-held note");

    sc55::midiPlayback::sendAllNotesOff ([&] (const uint8_t* bytes, int size)
    {
        if (synth.push (std::span (bytes, size_t (size))) != size_t (size))
            throw std::runtime_error ("Native engine rejected All Notes Off");
    });

    // Pause is expected to stop a pedal-held note immediately, rather than
    // wait for its programmed release envelope to decay naturally.
    render (3200);
    const auto voices = synth.state().parts[1].voices;
    std::printf ("Pause pedal-held regression: voices after 100 ms = %u\n", voices);
    if (voices != 0)
        throw std::runtime_error ("Pause All Notes Off left a pedal-held voice sounding");
    std::puts ("PASS: Pause All Notes Off silenced a pedal-held note");
    return 0;
}
