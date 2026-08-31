#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MidiFileData;

namespace rcpfile
{
/** Returns true when the buffer starts with the RCP v2 header. */
bool isRcpV2 (const std::vector<std::uint8_t>& data) noexcept;

/**
    Parses an RCP v2 sequence and expands it into the common MIDI-file event
    representation used by the plug-in.  The parser is deliberately kept free
    of JUCE so it can be tested without building the plug-in.
*/
bool loadRcpV2 (const std::vector<std::uint8_t>& data,
                MidiFileData& destination,
                std::string& error,
                const std::string& sourcePath = {});
}
