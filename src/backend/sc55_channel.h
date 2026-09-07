#pragma once
#include "sc55_midi.h"
#include "sc55_level.h"
#include <array>

namespace sc55
{
// Native MIDI channel values, independent of firmware RAM. This is the input
// state for voice parameter calculation, not a voice allocator or GS address map.
class ChannelControls
{
public:
    struct Channel
    {
        uint8_t program = 0;
        uint8_t volume = 100, expression = 127, pan = 64;
        uint8_t reverb = 40, chorus = 0;
        uint8_t rpnMsb = 127, rpnLsb = 127;
        bool nrpnSelected = false;
        int8_t coarseTuning = 0;
        bool softPedal = false;
    };
    const Channel& channel(unsigned index) const { return channels[index]; }
    void reset() noexcept { channels = {}; }

    // Returns false for events not implemented here. The engine must dispatch
    // these to note, GS, or other controller handling rather than lose them.
    bool apply(const MidiDecoder::Event& event) noexcept
    {
        if (event.kind != MidiDecoder::Kind::message) return false;
        auto& ch = channels[event.status & 15];
        if ((event.status & 0xf0) == 0xc0 && event.dataSize == 1)
        {
            ch.program = event.first;
            return true;
        }
        if ((event.status & 0xf0) != 0xb0 || event.dataSize != 2) return false;
        switch (event.first)
        {
            case 101: ch.rpnMsb = event.second; ch.nrpnSelected = false; break;
            case 100: ch.rpnLsb = event.second; ch.nrpnSelected = false; break;
            case 99: case 98:
                ch.nrpnSelected = true;
                return false; // NRPN number/value handling belongs to its dispatcher.
            case 6:
                if (ch.nrpnSelected || ch.rpnMsb != 0 || ch.rpnLsb != 2) return false;
                // v1.21 RPN coarse tuning is limited to +/-24 semitones.
                ch.coarseTuning = int8_t((event.second < 40 ? 40 : event.second > 88 ? 88 : event.second) - 64);
                break;
            case 7: ch.volume = event.second; break;
            // SC-55 v1.21 stores MIDI CC10=0 as 1 (GS SysEx pan has its own path).
            case 10: ch.pan = event.second == 0 ? 1 : event.second; break;
            case 11: ch.expression = event.second; break;
            case 67: ch.softPedal = event.second >= 64; break;
            case 91: ch.reverb = event.second; break;
            case 93: ch.chorus = event.second; break;
            default: return false;
        }
        return true;
    }
private:
    std::array<Channel, 16> channels {};
};

// Established v1.21 mappings: part+08 is CC7 volume (the legacy
// LevelInputs::velocity name does NOT mean note-on velocity), AB36 is CC11,
// and part+09/+0e/+0f are pan/chorus/reverb. Resolve MIDI-channel-to-part
// routing before calling. Master/tone controls, partial base pan and LFO
// state remain with their existing owners; refresh this before each update.
inline void ApplyChannelOutputControls(const ChannelControls::Channel& channel,
    LevelInputs& level,SpatialInputs& spatial) noexcept
{
    level.velocity = channel.volume;
    level.expression = channel.expression;
    spatial.pan = channel.pan;
    // SpatialInputs retains legacy reversed names: reverb is the LOW-byte
    // source (+0e), chorus is the HIGH-byte source (+0f). Match addresses.
    spatial.reverb = channel.chorus;
    spatial.chorus = channel.reverb;
}
}
