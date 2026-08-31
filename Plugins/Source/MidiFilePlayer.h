#pragma once

#include <array>
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

/**
    One decoded MAG background image.  Keeping the pixels as plain RGB data
    leaves file loading independent of JUCE; the standalone window creates a
    juce::Image only when the displayed frame changes.
*/
struct WrdBackgroundImage
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
    std::string filePath;
};

/**
    One complete text-screen snapshot from a companion .WRD file.

    WRD files are authored for an 80x25 MIMPI text screen.  The parser keeps
    the original bytes here because the files are normally Shift-JIS; the
    standalone window converts them on the message thread before painting.
*/
struct WrdDisplayFrame
{
    static constexpr std::size_t columns = 80;
    static constexpr std::size_t rows = 25;

    double seconds = 0.0;
    std::array<std::string, rows> lines;
    std::array<std::uint8_t, rows> colours {};
    int backgroundIndex = -1;
};

struct MidiFileData
{
    std::vector<MidiFileEvent> events;
    std::vector<WrdDisplayFrame> wrdFrames;
    std::vector<WrdBackgroundImage> wrdBackgrounds;
    std::string wrdFilePath;
    std::string wrdParseError;
    double songEndSeconds = 0.0;
    double lastBarSeconds = 2.0;

    /** Returns false and fills in `error` if the file is not a usable SMF. */
    bool load (const std::string& path, std::string& error, bool loadWrd = true);

    /** The song plus one bar of tail, so releases and reverb are not cut off. */
    double totalSeconds() const { return songEndSeconds + lastBarSeconds; }

    void clear()
    {
        events.clear();
        wrdFrames.clear();
        wrdBackgrounds.clear();
        wrdFilePath.clear();
        wrdParseError.clear();
        songEndSeconds = 0.0;
        lastBarSeconds = 2.0;
    }
};
