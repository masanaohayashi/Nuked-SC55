#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
    A Standard MIDI File flattened into one time-ordered event list.

    Events that share a tick keep the order they have in the file.  RPN and NRPN
    are a state machine - a select (CC 101/100 or 99/98) followed by a data entry
    (CC 6/38) - so reordering a pair silently loses the setting.  Sequencers that
    sort or de-duplicate same-tick controllers are exactly how a pitch bend range
    goes missing, which is why this parser is deliberately order-preserving.
*/
struct MidiFileEvent
{
    double seconds;
    std::vector<uint8_t> bytes;
};

struct MidiFileData
{
    std::vector<MidiFileEvent> events;
    double songEndSeconds = 0.0;
    double lastBarSeconds = 2.0;

    /** Returns false and fills in `error` if the file is not a usable SMF. */
    bool load (const std::string& path, std::string& error);

    /** The song plus one bar of tail, so releases and reverb are not cut off. */
    double totalSeconds() const { return songEndSeconds + lastBarSeconds; }

    void clear() { events.clear(); songEndSeconds = 0.0; lastBarSeconds = 2.0; }
};
