#pragma once
#include <array>
#include <cstdint>

namespace sc55
{
enum class PartParameter { program, volume, pan, reverb, chorus, keyShift, channel };
// Semantic readback for the editor. No CPU registers, PCM memory views or
// references into the live sound generator escape through this snapshot.
struct SynthState
{
    struct Part
    {
        uint8_t channel=0,program=0,bank=0,volume=0,expression=0,pan=0;
        uint8_t reverb=0,chorus=0,keyShift=64,voices=0;
        bool rhythm=false;
        bool muted=false;
        std::array<char,12> name{};
        uint16_t envelopeLevel=0; // Peak stereo envelope sum, not audio RMS.
    };
    std::array<Part,16> parts{}; // GS part order, not MIDI channel order.
    uint64_t renderedFrames=0;
    uint32_t activeVoiceMask=0;
    uint8_t selectedPart=0; // Display order 0..15; GS order is different.
    bool failed=false;
    bool allSelected=false,globalMuted=false;
    uint8_t masterVolume=127;
    uint8_t masterPan=64,masterKeyShift=64,reverbLevel=64,chorusLevel=64;
    std::array<uint8_t,16> displayText{};
    std::array<uint8_t,64> displayBitmap{};
    bool displayTextVisible=false,displayBitmapVisible=false;
};
}
