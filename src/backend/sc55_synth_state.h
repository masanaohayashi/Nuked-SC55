#pragma once
#include <array>
#include <cstdint>
#include "sc55_display_events.h"

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
    struct VoiceLevel {
        uint16_t left=0,right=0;
        uint8_t part=0;
        bool active=false;
    };
    // Audio publishes raw sound state once per voice. Display-only aggregation
    // is performed by the snapshot consumer, never by NativeSynth::state().
    std::array<VoiceLevel,24> voiceLevels{};
    void calculateDisplayLevels() noexcept
    {
        for(auto& part:parts) part.envelopeLevel=0;
        for(const auto& voice:voiceLevels) if(voice.active && voice.part<parts.size()) {
            const auto sum=unsigned(voice.left)+voice.right;
            const auto level=uint16_t(sum>65535u ? 65535u : sum);
            auto& peak=parts[voice.part].envelopeLevel;
            if(level>peak) peak=level;
        }
    }
    uint64_t renderedFrames=0;
    // PCM enable/key bits, not allocated or audible voice count. The reference
    // chip may keep these enabled after all notes have ended; use Part::voices
    // for musical ownership (and audio measurements for actual silence).
    uint32_t activeVoiceMask=0;
    uint8_t selectedPart=0; // Display order 0..15; GS order is different.
    bool failed=false;
    bool allSelected=false,globalMuted=false;
    bool soloEnabled=false;
    uint8_t masterVolume=127;
    uint8_t masterPan=64,masterKeyShift=64,reverbLevel=64,chorusLevel=64;
    MidiInputSettings midiInput;
    DisplayEvents displayEvents;
};
}
