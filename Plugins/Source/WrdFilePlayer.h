#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MidiFileData;

namespace wrdfile
{
/** A constant-tempo segment copied from the RCP playback timeline. */
struct TempoSegment
{
    std::int64_t startTick = 0;
    double startSeconds = 0.0;
    double bpm = 120.0;
};

/** The small amount of RCP timing information needed by the WRD scheduler. */
struct Timing
{
    int timeBase = 48;
    int beatNumerator = 4;
    int beatDenominator = 4;
    std::vector<TempoSegment> segments;

    double secondsAtTick (std::int64_t tick) const noexcept;
};

/**
    Loads the optional same-basename .WRD companion and its MAG background
    images for an RCP file.

    Missing companions are normal and return true.  A present but malformed
    companion or image is reported through `warning` and does not invalidate
    the RCP.
*/
bool loadForRcp (const std::string& rcpPath,
                 const Timing& timing,
                 MidiFileData& destination,
                 std::string& warning);
}
