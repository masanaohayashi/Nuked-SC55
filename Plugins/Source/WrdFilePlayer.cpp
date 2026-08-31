#include "WrdFilePlayer.h"

#include "MidiFilePlayer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr std::size_t maximumWrdFileSize = 8 * 1024 * 1024;
constexpr std::size_t maximumMagFileSize = 64 * 1024 * 1024;
constexpr std::size_t maximumDecodedMagBytes = 64 * 1024 * 1024;
constexpr int wrdTicksPerMeasure = 96;

struct WrdToken
{
    bool command = false;
    std::string name;
    std::string arguments;
    std::string text;
    bool continuing = false;
};

struct TextCell
{
    std::string bytes { " " };
    std::uint8_t colour = 23;
};

struct TextScreen
{
    std::array<TextCell, WrdDisplayFrame::columns * WrdDisplayFrame::rows> cells;
    std::array<std::uint8_t, WrdDisplayFrame::rows> lineColours {};
    int x = 1;
    int y = 1;
    int colour = 23;
    int deltaSteps = 0;
    int wmode = 0;
    int bar = 1;
    int step = 0;
    int offsetBars = 0;
    bool textOn = true;
    std::array<int, 2> graphicsPages { -1, -1 };
    int graphicsPage = 0;
    int backgroundIndex = -1;
    bool graphicsOn = true;

    TextScreen()
    {
        reset();
    }

    void reset()
    {
        for (auto& cell : cells)
        {
            cell.bytes = " ";
            cell.colour = 23;
        }

        lineColours.fill (23);
        x = 1;
        y = 1;
        colour = 23;
        deltaSteps = 0;
        wmode = 0;
        bar = 1;
        step = 0;
        offsetBars = 0;
        textOn = true;
        graphicsPages = { -1, -1 };
        graphicsPage = 0;
        backgroundIndex = -1;
        graphicsOn = true;
    }

    static bool isShiftJisLead (const std::uint8_t value) noexcept
    {
        return (value >= 0x81 && value <= 0x9f)
            || (value >= 0xe0 && value <= 0xfc);
    }

    static bool isAsciiSpace (const std::uint8_t value) noexcept
    {
        return value == static_cast<std::uint8_t> (' ')
            || value == static_cast<std::uint8_t> ('\t');
    }

    std::size_t index() const noexcept
    {
        return static_cast<std::size_t> (y - 1) * WrdDisplayFrame::columns
             + static_cast<std::size_t> (x - 1);
    }

    void clearCell (int column, int row, std::uint8_t fillColour = 23)
    {
        if (column < 1 || column > static_cast<int> (WrdDisplayFrame::columns)
            || row < 1 || row > static_cast<int> (WrdDisplayFrame::rows))
            return;

        auto& cell = cells[static_cast<std::size_t> (row - 1) * WrdDisplayFrame::columns
                           + static_cast<std::size_t> (column - 1)];
        cell.bytes = " ";
        cell.colour = fillColour;
    }

    void clearAll()
    {
        for (auto& cell : cells)
        {
            cell.bytes = " ";
            cell.colour = 23;
        }

        lineColours.fill (23);
    }

    void clearRange (int firstColumn, int firstRow,
                     int lastColumn, int lastRow,
                     std::uint8_t fillColour = 23)
    {
        firstColumn = std::clamp (firstColumn, 1, static_cast<int> (WrdDisplayFrame::columns));
        lastColumn = std::clamp (lastColumn, 1, static_cast<int> (WrdDisplayFrame::columns));
        firstRow = std::clamp (firstRow, 1, static_cast<int> (WrdDisplayFrame::rows));
        lastRow = std::clamp (lastRow, 1, static_cast<int> (WrdDisplayFrame::rows));
        if (firstColumn > lastColumn || firstRow > lastRow)
            return;

        for (int row = firstRow; row <= lastRow; ++row)
            for (int column = firstColumn; column <= lastColumn; ++column)
                clearCell (column, row, fillColour);
    }

    void locate (int column, int row)
    {
        x = std::clamp (column, 1, static_cast<int> (WrdDisplayFrame::columns));
        y = std::clamp (row, 1, static_cast<int> (WrdDisplayFrame::rows));
    }

    void scrollUp()
    {
        for (int row = 1; row < static_cast<int> (WrdDisplayFrame::rows); ++row)
        {
            for (int column = 1; column <= static_cast<int> (WrdDisplayFrame::columns); ++column)
            {
                const auto source = static_cast<std::size_t> (row) * WrdDisplayFrame::columns
                                  + static_cast<std::size_t> (column - 1);
                auto& destination = cells[static_cast<std::size_t> (row - 1)
                                           * WrdDisplayFrame::columns
                                           + static_cast<std::size_t> (column - 1)];
                destination = cells[source];
            }
            lineColours[static_cast<std::size_t> (row - 1)] = lineColours[static_cast<std::size_t> (row)];
        }

        clearRange (1, static_cast<int> (WrdDisplayFrame::rows),
                    static_cast<int> (WrdDisplayFrame::columns),
                    static_cast<int> (WrdDisplayFrame::rows));
        lineColours.back() = 23;
    }

    void nextLine()
    {
        x = 1;
        if (y < static_cast<int> (WrdDisplayFrame::rows))
            ++y;
        else
            scrollUp();
    }

    void moveRight()
    {
        if (x < static_cast<int> (WrdDisplayFrame::columns))
            ++x;
        else
            nextLine();
    }

    void putBytes (const std::string& bytes)
    {
        if (! textOn || bytes.empty())
            return;

        if (x > static_cast<int> (WrdDisplayFrame::columns))
            nextLine();

        const auto cellIndex = index();
        auto& cell = cells[cellIndex];
        cell.bytes = bytes;
        cell.colour = static_cast<std::uint8_t> (std::clamp (colour, 0, 47));
        lineColours[static_cast<std::size_t> (y - 1)] = cell.colour;

        // A Japanese double-byte character occupies two MIMPI columns.  The
        // second cell stays blank but is coloured so the window keeps the
        // original character-cell spacing.
        if (bytes.size() == 2 && x < static_cast<int> (WrdDisplayFrame::columns))
        {
            auto& secondCell = cells[cellIndex + 1];
            secondCell.bytes = " ";
            secondCell.colour = cell.colour;
            x += 2;
        }
        else
        {
            moveRight();
        }
    }

    void putByte (const std::uint8_t value)
    {
        if (value == '\n')
        {
            nextLine();
            return;
        }

        if (value == '\r')
        {
            x = 1;
            return;
        }

        if (value == '\b')
        {
            x = std::max (1, x - 1);
            return;
        }

        if (value == '\t')
        {
            const auto nextTab = ((x - 1) / 8 + 1) * 8 + 1;
            locate (std::min (nextTab, static_cast<int> (WrdDisplayFrame::columns)), y);
            return;
        }

        if (value < 0x20)
            return;

        putBytes (std::string (1, static_cast<char> (value)));
    }

    void applyEscape (const std::string& sequence)
    {
        if (sequence.empty())
            return;

        // @ESC(str) supplies the part after ESC[, while raw WRD text may
        // contain the complete ESC[... sequence.
        std::string body = sequence;
        if (body[0] == '\x1b')
        {
            body.erase (body.begin());
            if (! body.empty() && body.front() == '[')
                body.erase (body.begin());
        }
        else if (! body.empty() && body.front() == '[')
        {
            body.erase (body.begin());
        }

        if (body.empty())
            return;

        const auto final = body.back();
        body.pop_back();
        if (final == 'J')
        {
            const auto value = body.empty() ? 0 : std::atoi (body.c_str());
            if (value == 2)
                clearAll();
            else if (value == 0)
                clearRange (x, y, static_cast<int> (WrdDisplayFrame::columns),
                            static_cast<int> (WrdDisplayFrame::rows));
            return;
        }

        if (final == 'K')
        {
            const auto value = body.empty() ? 0 : std::atoi (body.c_str());
            if (value == 2)
                clearRange (1, y, static_cast<int> (WrdDisplayFrame::columns), y);
            else if (value == 1)
                clearRange (1, y, x, y);
            else
                clearRange (x, y, static_cast<int> (WrdDisplayFrame::columns), y);
            return;
        }

        if (final == 'H' || final == 'f')
        {
            int row = 1;
            int column = 1;
            const auto separator = body.find (';');
            if (separator == std::string::npos)
            {
                if (! body.empty())
                    row = std::max (1, std::atoi (body.c_str()));
            }
            else
            {
                row = std::max (1, std::atoi (body.substr (0, separator).c_str()));
                column = std::max (1, std::atoi (body.substr (separator + 1).c_str()));
            }
            locate (column, row);
            return;
        }

        const auto distance = std::max (1, body.empty() ? 1 : std::atoi (body.c_str()));
        switch (final)
        {
            case 'A': locate (x, y - distance); break;
            case 'B': locate (x, y + distance); break;
            case 'C': locate (x + distance, y); break;
            case 'D': locate (x - distance, y); break;
            case 'E': locate (1, y + distance); break;
            case 'F': locate (1, y - distance); break;
            default: break;
        }
    }

    void writeLyrics (const std::string& text)
    {
        for (std::size_t indexInText = 0; indexInText < text.size(); ++indexInText)
        {
            const auto value = static_cast<std::uint8_t> (text[indexInText]);

            if (value == 0x1b)
            {
                auto end = indexInText + 1;
                while (end < text.size()
                       && ! std::isalpha (static_cast<unsigned char> (text[end])))
                    ++end;
                if (end < text.size())
                {
                    applyEscape (text.substr (indexInText, end - indexInText + 1));
                    indexInText = end;
                }
                continue;
            }

            if (wmode == 1)
            {
                if (value == '_' || value == '|')
                    continue;

                if (value == '\\')
                {
                    if (indexInText + 1 < text.size())
                        putByte (static_cast<std::uint8_t> (text[++indexInText]));
                    continue;
                }
            }

            if (isShiftJisLead (value) && indexInText + 1 < text.size())
            {
                putBytes (text.substr (indexInText, 2));
                ++indexInText;
            }
            else
            {
                putByte (value);
            }
        }

        // A physical WRD line ends the current text line.  This also makes
        // blank lines useful, as they are timing-bearing lyric records.
        nextLine();
    }

    void scroll (const std::vector<int>& values)
    {
        const auto firstColumn = values.size() > 0 ? values[0] : 1;
        const auto firstRow = values.size() > 1 ? values[1] : 1;
        const auto lastColumn = values.size() > 2 ? values[2]
                                                  : static_cast<int> (WrdDisplayFrame::columns);
        const auto lastRow = values.size() > 3 ? values[3]
                                               : static_cast<int> (WrdDisplayFrame::rows);
        const auto mode = values.size() > 4 ? values[4] : 0;

        if (mode == 0 && firstColumn == 1 && lastColumn == static_cast<int> (WrdDisplayFrame::columns))
        {
            if (firstRow == 1 && lastRow == static_cast<int> (WrdDisplayFrame::rows))
            {
                scrollUp();
                return;
            }

            const auto top = std::clamp (firstRow, 1, static_cast<int> (WrdDisplayFrame::rows));
            const auto bottom = std::clamp (lastRow, top, static_cast<int> (WrdDisplayFrame::rows));
            for (int row = top; row < bottom; ++row)
            {
                for (int column = 1; column <= static_cast<int> (WrdDisplayFrame::columns); ++column)
                {
                    auto& destination = cells[static_cast<std::size_t> (row - 1)
                                               * WrdDisplayFrame::columns
                                               + static_cast<std::size_t> (column - 1)];
                    const auto source = static_cast<std::size_t> (row) * WrdDisplayFrame::columns
                                      + static_cast<std::size_t> (column - 1);
                    destination = cells[source];
                }
                lineColours[static_cast<std::size_t> (row - 1)] = lineColours[static_cast<std::size_t> (row)];
            }
            clearRange (1, bottom, static_cast<int> (WrdDisplayFrame::columns), bottom);
        }
    }
};

bool isLineBreak (const std::uint8_t value) noexcept
{
    return value == '\r' || value == '\n' || value == 0x85;
}

bool isAsciiLetter (const std::uint8_t value) noexcept
{
    return (value >= static_cast<std::uint8_t> ('a') && value <= static_cast<std::uint8_t> ('z'))
        || (value >= static_cast<std::uint8_t> ('A') && value <= static_cast<std::uint8_t> ('Z'));
}

std::string lowerAscii (const std::string& value)
{
    auto result = value;
    for (auto& character : result)
        character = static_cast<char> (std::tolower (static_cast<unsigned char> (character)));
    return result;
}

std::size_t skipAsciiSpace (const std::string& data, std::size_t position, std::size_t end)
{
    while (position < end
           && (data[position] == ' ' || data[position] == '\t'))
        ++position;
    return position;
}

bool readFileContents (const std::string& path,
                       std::string& contents,
                       const std::size_t maximumSize = maximumWrdFileSize)
{
    contents.clear();
    auto* file = std::fopen (path.c_str(), "rb");
    if (file == nullptr)
        return false;

    std::fseek (file, 0, SEEK_END);
    const auto size = std::ftell (file);
    std::rewind (file);
    if (size < 0 || static_cast<std::size_t> (size) > maximumSize)
    {
        std::fclose (file);
        return false;
    }

    contents.resize (static_cast<std::size_t> (size));
    const auto loaded = contents.empty()
        || std::fread (contents.data(), 1, contents.size(), file) == contents.size();
    std::fclose (file);
    if (! loaded)
    {
        contents.clear();
        return false;
    }
    return true;
}

bool fileExists (const std::string& path)
{
    auto* file = std::fopen (path.c_str(), "rb");
    if (file == nullptr)
        return false;
    std::fclose (file);
    return true;
}

bool readLittleEndian16 (const std::string& data,
                         const std::size_t offset,
                         std::uint16_t& value)
{
    if (offset > data.size() || sizeof (std::uint16_t) > data.size() - offset)
        return false;

    const auto byteAt = [&data] (const std::size_t index)
    {
        return static_cast<std::uint32_t> (static_cast<unsigned char> (data[index]));
    };
    value = static_cast<std::uint16_t> (byteAt (offset)
                                      | (byteAt (offset + 1) << 8));
    return true;
}

bool readLittleEndian32 (const std::string& data,
                         const std::size_t offset,
                         std::uint32_t& value)
{
    if (offset > data.size() || sizeof (std::uint32_t) > data.size() - offset)
        return false;

    const auto byteAt = [&data] (const std::size_t index)
    {
        return static_cast<std::uint32_t> (static_cast<unsigned char> (data[index]));
    };
    value = byteAt (offset)
          | (byteAt (offset + 1) << 8)
          | (byteAt (offset + 2) << 16)
          | (byteAt (offset + 3) << 24);
    return true;
}

bool checkedMagRange (const std::string& data,
                      const std::size_t base,
                      const std::uint32_t relativeOffset,
                      const std::uint32_t size,
                      std::size_t& absoluteOffset)
{
    if (base > data.size()
        || static_cast<std::size_t> (relativeOffset) > data.size() - base)
        return false;

    absoluteOffset = base + static_cast<std::size_t> (relativeOffset);
    return static_cast<std::size_t> (size) <= data.size() - absoluteOffset;
}

/**
    Decode the packed MAKI02/MAG format into RGB pixels.

    MAG has a persistent per-x-unit copy flag.  Flag A tells us which entries
    in flag B change on each row; a non-zero flag B nibble copies a previously
    decoded four-pixel (16 colour) or two-pixel (256 colour) unit.  This is
    intentionally kept here, on the file-loading side, rather than in the
    audio callback or the component's paint method.
*/
bool decodeMagImage (const std::string& data,
                     WrdBackgroundImage& destination,
                     std::string& error)
{
    destination = {};
    error.clear();

    const auto fail = [&error] (const char* message)
    {
        error = message;
        return false;
    };

    if (data.size() < 8 || data.compare (0, 8, "MAKI02  ") != 0)
        return fail ("MAG magic is not MAKI02");

    // The fixed MAKI02 preamble is 31 bytes; the memo ends at the first
    // Ctrl-Z and the 32-byte image header follows it.
    const auto memoEnd = data.find (static_cast<char> (0x1a), 31);
    if (memoEnd == std::string::npos)
        return fail ("MAG memo terminator is missing");

    const auto headerOffset = memoEnd + 1;
    if (headerOffset > data.size() || 32 > data.size() - headerOffset)
        return fail ("MAG header is truncated");
    if (static_cast<unsigned char> (data[headerOffset]) != 0)
        return fail ("MAG header marker is invalid");

    const auto screenMode = static_cast<std::uint8_t> (
        static_cast<unsigned char> (data[headerOffset + 3]));
    const auto palette256 = (screenMode & 0x80u) != 0;
    const auto pixelUnit = palette256 ? 4u : 8u;
    const auto colourCount = palette256 ? 256u : 16u;

    std::uint16_t startX = 0;
    std::uint16_t startY = 0;
    std::uint16_t endX = 0;
    std::uint16_t endY = 0;
    if (! readLittleEndian16 (data, headerOffset + 4, startX)
        || ! readLittleEndian16 (data, headerOffset + 6, startY)
        || ! readLittleEndian16 (data, headerOffset + 8, endX)
        || ! readLittleEndian16 (data, headerOffset + 10, endY))
        return fail ("MAG dimensions are truncated");

    const auto firstXUnit = static_cast<std::uint32_t> (startX) / pixelUnit;
    const auto lastXUnit = static_cast<std::uint32_t> (endX) / pixelUnit;
    if (lastXUnit < firstXUnit || endY < startY)
        return fail ("MAG dimensions are invalid");

    const auto width64 = static_cast<std::uint64_t> (lastXUnit - firstXUnit + 1u)
                       * pixelUnit;
    const auto height64 = static_cast<std::uint64_t> (endY - startY) + 1u;
    const auto outputHeight64 = (screenMode & 1u) != 0
        ? height64 * 2u
        : height64;
    const auto rgbBytes64 = width64 * outputHeight64 * 3u;

    if (width64 == 0 || outputHeight64 == 0
        || rgbBytes64 > maximumDecodedMagBytes
        || width64 > static_cast<std::uint64_t> (std::numeric_limits<int>::max())
        || outputHeight64 > static_cast<std::uint64_t> (std::numeric_limits<int>::max()))
        return fail ("MAG image is too large");

    std::uint32_t flagAOffset = 0;
    std::uint32_t flagBOffset = 0;
    std::uint32_t flagBSize = 0;
    std::uint32_t pixelOffset = 0;
    std::uint32_t pixelSize = 0;
    if (! readLittleEndian32 (data, headerOffset + 12, flagAOffset)
        || ! readLittleEndian32 (data, headerOffset + 16, flagBOffset)
        || ! readLittleEndian32 (data, headerOffset + 20, flagBSize)
        || ! readLittleEndian32 (data, headerOffset + 24, pixelOffset)
        || ! readLittleEndian32 (data, headerOffset + 28, pixelSize)
        || flagBOffset < flagAOffset)
        return fail ("MAG data offsets are invalid");

    const auto flagASize = flagBOffset - flagAOffset;
    std::size_t paletteOffset = 0;
    if (headerOffset > data.size()
        || 32 > data.size() - headerOffset
        || static_cast<std::size_t> (colourCount * 3u)
               > data.size() - (headerOffset + 32))
        return fail ("MAG palette is truncated");
    paletteOffset = headerOffset + 32;

    std::size_t flagAStart = 0;
    std::size_t flagBStart = 0;
    std::size_t pixelsStart = 0;
    if (! checkedMagRange (data, headerOffset, flagAOffset, flagASize, flagAStart)
        || ! checkedMagRange (data, headerOffset, flagBOffset, flagBSize, flagBStart)
        || ! checkedMagRange (data, headerOffset, pixelOffset, pixelSize, pixelsStart))
        return fail ("MAG compressed data is outside the file");

    const auto width = static_cast<std::size_t> (width64);
    const auto height = static_cast<std::size_t> (height64);
    const auto numXUnits = width / pixelUnit;
    const auto expectedFlagBits = static_cast<std::uint64_t> (numXUnits) * height;
    if (static_cast<std::uint64_t> (flagASize) * 8u < expectedFlagBits)
        return fail ("MAG flag A data is truncated");

    const auto rgbBytes = static_cast<std::size_t> (rgbBytes64);
    std::vector<std::uint8_t> decoded (width * height * 3u, 0);
    std::vector<std::uint8_t> lineFlags (numXUnits, 0);
    std::size_t flagABit = 0;
    std::size_t flagBPosition = 0;
    std::size_t pixelPosition = 0;

    constexpr std::array<int, 16> copyX {
        0, 1, 2, 4, 0, 1, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0
    };
    constexpr std::array<int, 16> copyY {
        0, 0, 0, 0, 1, 1, 2, 2, 2, 4, 4, 4, 8, 8, 8, 16
    };
    const auto copyPixels = palette256 ? 2u : 4u;

    const auto putPalettePixel = [&] (const std::size_t x,
                                      const std::size_t y,
                                      const std::uint8_t colour)
    {
        if (x >= width || y >= height || colour >= colourCount)
            return false;

        const auto source = paletteOffset + static_cast<std::size_t> (colour) * 3u;
        const auto target = (y * width + x) * 3u;
        // Palette entries are stored in GRB order in MAG files.
        decoded[target + 0] = static_cast<std::uint8_t> (
            static_cast<unsigned char> (data[source + 1]));
        decoded[target + 1] = static_cast<std::uint8_t> (
            static_cast<unsigned char> (data[source + 0]));
        decoded[target + 2] = static_cast<std::uint8_t> (
            static_cast<unsigned char> (data[source + 2]));
        return true;
    };

    const auto readPixelByte = [&] (std::uint8_t& value)
    {
        if (pixelPosition >= pixelSize)
            return false;
        value = static_cast<std::uint8_t> (
            static_cast<unsigned char> (data[pixelsStart + pixelPosition++]));
        return true;
    };

    for (std::size_t row = 0; row < height; ++row)
    {
        for (auto& lineFlag : lineFlags)
        {
            if (flagABit >= expectedFlagBits)
                return fail ("MAG flag A bit stream ended early");

            const auto flagByte = static_cast<std::uint8_t> (
                static_cast<unsigned char> (data[flagAStart + flagABit / 8u]));
            const auto flagChanged = (flagByte & (0x80u >> (flagABit & 7u))) != 0;
            ++flagABit;
            if (flagChanged)
            {
                if (flagBPosition >= flagBSize)
                    return fail ("MAG flag B data is truncated");
                lineFlag ^= static_cast<std::uint8_t> (
                    static_cast<unsigned char> (data[flagBStart + flagBPosition++]));
            }
        }

        std::size_t destinationX = 0;
        const auto decodeNibble = [&] (const std::uint8_t flag)
        {
            if (flag == 0)
            {
                if (palette256)
                {
                    for (unsigned int index = 0; index < 2; ++index)
                    {
                        std::uint8_t colour = 0;
                        if (! readPixelByte (colour)
                            || ! putPalettePixel (destinationX++, row, colour))
                            return false;
                    }
                }
                else
                {
                    for (unsigned int index = 0; index < 2; ++index)
                    {
                        std::uint8_t packed = 0;
                        if (! readPixelByte (packed)
                            || ! putPalettePixel (destinationX++, row,
                                                  static_cast<std::uint8_t> (packed >> 4))
                            || ! putPalettePixel (destinationX++, row,
                                                  static_cast<std::uint8_t> (packed & 0x0f)))
                            return false;
                    }
                }
                return true;
            }

            const auto sourceXDelta = static_cast<std::size_t> (copyX[flag]) * copyPixels;
            const auto sourceYDelta = static_cast<std::size_t> (copyY[flag]);
            if (sourceXDelta > destinationX || sourceYDelta > row
                || destinationX + copyPixels > width)
                return false;

            const auto sourceX = destinationX - sourceXDelta;
            const auto sourceY = row - sourceYDelta;
            if (sourceX + copyPixels > width)
                return false;

            for (unsigned int pixel = 0; pixel < copyPixels; ++pixel)
            {
                const auto source = (sourceY * width + sourceX + pixel) * 3u;
                const auto target = (row * width + destinationX + pixel) * 3u;
                decoded[target + 0] = decoded[source + 0];
                decoded[target + 1] = decoded[source + 1];
                decoded[target + 2] = decoded[source + 2];
            }
            destinationX += copyPixels;
            return true;
        };

        for (const auto lineFlag : lineFlags)
        {
            if (! decodeNibble (static_cast<std::uint8_t> (lineFlag >> 4))
                || ! decodeNibble (static_cast<std::uint8_t> (lineFlag & 0x0f)))
                return fail ("MAG pixel copy data is invalid");
        }

        if (destinationX != width)
            return fail ("MAG row width is invalid");
    }

    std::vector<std::uint8_t> output (rgbBytes, 0);
    const auto outputHeight = static_cast<std::size_t> (outputHeight64);
    const auto rowBytes = width * 3u;
    for (std::size_t row = 0; row < height; ++row)
    {
        const auto source = decoded.data() + row * rowBytes;
        const auto firstOutputRow = (screenMode & 1u) != 0 ? row * 2u : row;
        const auto repeatRows = (screenMode & 1u) != 0 ? 2u : 1u;
        for (std::size_t repeat = 0; repeat < repeatRows; ++repeat)
        {
            const auto target = output.data() + (firstOutputRow + repeat) * rowBytes;
            std::copy_n (source, rowBytes, target);
        }
    }

    destination.width = static_cast<int> (width);
    destination.height = static_cast<int> (outputHeight);
    destination.rgb = std::move (output);
    return true;
}

std::string companionPath (const std::string& rcpPath, const char* extension)
{
    const auto slash = rcpPath.find_last_of ("/\\");
    const auto dot = rcpPath.find_last_of ('.');
    const auto stem = dot != std::string::npos && (slash == std::string::npos || dot > slash)
                    ? rcpPath.substr (0, dot)
                    : rcpPath;
    return stem + "." + extension;
}

std::string findCompanion (const std::string& rcpPath)
{
    const auto lower = companionPath (rcpPath, "wrd");
    if (fileExists (lower))
        return lower;

    const auto upper = companionPath (rcpPath, "WRD");
    if (fileExists (upper))
        return upper;

    return {};
}

std::string findMagCompanion (const std::string& rcpPath)
{
    for (const auto* extension : { "MAG", "Mag", "mag" })
    {
        const auto candidate = companionPath (rcpPath, extension);
        if (fileExists (candidate))
            return candidate;
    }

    return {};
}

std::string trimAscii (std::string value)
{
    const auto isSpace = [] (const char character)
    {
        return character == ' ' || character == '\t';
    };

    while (! value.empty() && isSpace (value.front()))
        value.erase (value.begin());
    while (! value.empty() && isSpace (value.back()))
        value.pop_back();

    if (value.size() >= 2
        && ((value.front() == '"' && value.back() == '"')
            || (value.front() == '\'' && value.back() == '\'')))
    {
        value = value.substr (1, value.size() - 2);
    }
    return value;
}

std::string findMagPath (const std::string& wrdPath,
                         const std::string& requestedName)
{
    const auto requested = trimAscii (requestedName);
    if (requested.empty())
        return {};

    const auto isAbsolute = requested.front() == '/' || requested.front() == '\\'
                         || (requested.size() > 1 && requested[1] == ':');
    const auto wrdDirectoryEnd = wrdPath.find_last_of ("/\\");
    const auto directory = wrdDirectoryEnd == std::string::npos
        ? std::string {}
        : wrdPath.substr (0, wrdDirectoryEnd + 1);
    const auto basePath = isAbsolute ? requested : directory + requested;
    const auto extensionDot = basePath.find_last_of ('.');
    const auto lastSlash = basePath.find_last_of ("/\\");
    const auto hasExtension = extensionDot != std::string::npos
                           && (lastSlash == std::string::npos || extensionDot > lastSlash);
    const auto stem = hasExtension ? basePath.substr (0, extensionDot) : basePath;

    const auto candidates = std::array<std::string, 4> {
        basePath,
        stem + ".MAG",
        stem + ".Mag",
        stem + ".mag"
    };
    for (const auto& candidate : candidates)
    {
        if (fileExists (candidate))
            return candidate;
    }
    return {};
}

void appendWarning (std::string& warning, const std::string& message)
{
    if (message.empty())
        return;
    if (! warning.empty())
        warning += "; ";
    warning += message;
}

struct WrdAssetStore
{
    const std::string& wrdPath;
    MidiFileData& destination;
    std::string& warning;

    int loadNamed (const std::string& name)
    {
        const auto path = findMagPath (wrdPath, name);
        if (path.empty())
        {
            appendWarning (warning, "MAG companion could not be found: " + trimAscii (name));
            return -1;
        }
        return loadPath (path);
    }

    int loadPath (const std::string& path)
    {
        for (std::size_t index = 0; index < destination.wrdBackgrounds.size(); ++index)
        {
            if (destination.wrdBackgrounds[index].filePath == path)
                return static_cast<int> (index);
        }

        std::string contents;
        if (! readFileContents (path, contents, maximumMagFileSize))
        {
            appendWarning (warning, "MAG companion could not be read: " + path);
            return -1;
        }

        WrdBackgroundImage image;
        std::string error;
        if (! decodeMagImage (contents, image, error))
        {
            appendWarning (warning, "MAG companion is invalid (" + path + "): " + error);
            return -1;
        }

        image.filePath = path;
        destination.wrdBackgrounds.push_back (std::move (image));
        return static_cast<int> (destination.wrdBackgrounds.size() - 1);
    }
};

std::size_t findLineEnd (const std::string& data, std::size_t start)
{
    std::size_t position = start;
    while (position < data.size())
    {
        const auto value = static_cast<std::uint8_t> (data[position]);
        if (isLineBreak (value))
            break;
        if (TextScreen::isShiftJisLead (value) && position + 1 < data.size())
        {
            position += 2;
            continue;
        }
        ++position;
    }
    return position;
}

std::size_t skipLineBreaks (const std::string& data, std::size_t position)
{
    if (position >= data.size())
        return position;

    if (data[position] == '\r' && position + 1 < data.size() && data[position + 1] == '\n')
        return position + 2;
    return position + 1;
}

bool parseWrdTokens (const std::string& data,
                     std::vector<WrdToken>& tokens,
                     std::string& warning)
{
    tokens.clear();
    warning.clear();

    std::size_t lineStart = 0;
    while (lineStart < data.size())
    {
        const auto lineEnd = findLineEnd (data, lineStart);
        std::size_t cursor = lineStart;
        bool emittedToken = false;

        while (cursor < lineEnd)
        {
            const auto candidate = skipAsciiSpace (data, cursor, lineEnd);
            if (candidate < lineEnd
                && (data[candidate] == '@' || data[candidate] == '^'))
            {
                cursor = candidate;
                const auto marker = data[cursor++];
                const auto nameStart = cursor;
                while (cursor < lineEnd
                       && isAsciiLetter (static_cast<std::uint8_t> (data[cursor])))
                    ++cursor;

                WrdToken token;
                token.command = true;
                token.name = lowerAscii (std::string (1, marker)
                                         + data.substr (nameStart, cursor - nameStart));
                cursor = skipAsciiSpace (data, cursor, lineEnd);

                if (token.name == "@rem" || token.name == "@remark")
                {
                    // Comments are still timing records in the original
                    // parser, but their remainder is never another token.
                    token.continuing = false;
                    tokens.push_back (std::move (token));
                    emittedToken = true;
                    cursor = lineEnd;
                    continue;
                }

                if (cursor < lineEnd && data[cursor] == '(')
                {
                    const auto argumentStart = ++cursor;
                    int depth = 1;
                    while (cursor < lineEnd && depth > 0)
                    {
                        if (data[cursor] == '(')
                            ++depth;
                        else if (data[cursor] == ')')
                            --depth;
                        ++cursor;
                    }

                    if (depth != 0)
                    {
                        warning = "WRD command has an unterminated argument list";
                        token.arguments = data.substr (argumentStart, lineEnd - argumentStart);
                    }
                    else
                    {
                        token.arguments = data.substr (argumentStart,
                                                        cursor - argumentStart - 1);
                    }
                }

                auto afterCommand = skipAsciiSpace (data, cursor, lineEnd);
                const bool hasSemicolon = afterCommand < lineEnd && data[afterCommand] == ';';
                if (hasSemicolon)
                    ++afterCommand;
                const auto nextToken = skipAsciiSpace (data, afterCommand, lineEnd);
                token.continuing = hasSemicolon || nextToken < lineEnd;
                tokens.push_back (std::move (token));
                emittedToken = true;
                cursor = afterCommand;
                continue;
            }

            const auto textStart = cursor;
            auto textEnd = textStart;
            while (textEnd < lineEnd)
            {
                const auto value = static_cast<std::uint8_t> (data[textEnd]);
                if (value == '@' || value == '^')
                    break;
                if (TextScreen::isShiftJisLead (value) && textEnd + 1 < lineEnd)
                    textEnd += 2;
                else
                    ++textEnd;
            }

            if (textEnd == textStart)
            {
                ++cursor;
                continue;
            }

            WrdToken token;
            token.text = data.substr (textStart, textEnd - textStart);
            auto trimEnd = token.text.size();
            while (trimEnd > 0 && TextScreen::isAsciiSpace (static_cast<std::uint8_t> (token.text[trimEnd - 1])))
                --trimEnd;
            if (trimEnd > 0 && token.text[trimEnd - 1] == ';')
            {
                token.text.erase (trimEnd - 1, 1);
                token.continuing = true;
            }
            else
            {
                token.continuing = false;
            }

            token.continuing = token.continuing || textEnd < lineEnd;
            tokens.push_back (std::move (token));
            emittedToken = true;
            cursor = textEnd;
        }

        // WrdFile treats an empty physical line as a lyric record.  Keeping
        // it matters for older WRD files such as GENKI^3.WRD.
        if (! emittedToken && lineStart == lineEnd)
            tokens.push_back ({ false, {}, {}, {}, false });

        if (lineEnd >= data.size())
            break;
        lineStart = skipLineBreaks (data, lineEnd);
    }

    return ! tokens.empty();
}

std::vector<int> parseIntegers (const std::string& arguments)
{
    std::vector<int> result;
    std::size_t position = 0;
    while (position < arguments.size())
    {
        const auto value = static_cast<unsigned char> (arguments[position]);
        if (std::isdigit (value) || (value == '-' && position + 1 < arguments.size()
                                      && std::isdigit (static_cast<unsigned char> (arguments[position + 1]))))
        {
            char* end = nullptr;
            const auto parsed = std::strtol (arguments.c_str() + position, &end, 10);
            if (end != nullptr && end != arguments.c_str() + position)
            {
                result.push_back (static_cast<int> (std::clamp<long> (
                    parsed, std::numeric_limits<int>::min(), std::numeric_limits<int>::max())));
                position = static_cast<std::size_t> (end - arguments.c_str());
                continue;
            }
        }
        ++position;
    }
    return result;
}

double tickForPosition (const TextScreen& screen, const wrdfile::Timing& timing)
{
    const auto ticksPerBar = std::max (1.0,
        static_cast<double> (std::max (1, timing.timeBase))
        * std::max (1, timing.beatNumerator) * 4.0
        / std::max (1, timing.beatDenominator));
    const auto bar = std::max (1, screen.bar + screen.offsetBars);
    const auto step = std::clamp (screen.step, 0, wrdTicksPerMeasure - 1);
    return static_cast<double> (bar - 1) * ticksPerBar
         + static_cast<double> (step) * ticksPerBar / wrdTicksPerMeasure;
}

void advanceWrdPosition (TextScreen& screen)
{
    if (screen.deltaSteps <= 0)
    {
        ++screen.bar;
        screen.step = 0;
        return;
    }

    const auto nextStep = screen.step + screen.deltaSteps + 1;
    screen.bar += nextStep / wrdTicksPerMeasure;
    screen.step = nextStep % wrdTicksPerMeasure;
}

WrdDisplayFrame makeFrame (const TextScreen& screen,
                           const wrdfile::Timing& timing)
{
    WrdDisplayFrame frame;
    frame.seconds = timing.secondsAtTick (
        static_cast<std::int64_t> (std::llround (tickForPosition (screen, timing))));
    frame.colours = screen.lineColours;
    frame.backgroundIndex = screen.backgroundIndex;

    for (std::size_t row = 0; row < WrdDisplayFrame::rows; ++row)
    {
        auto& line = frame.lines[row];
        line.reserve (WrdDisplayFrame::columns);
        for (std::size_t column = 0; column < WrdDisplayFrame::columns; ++column)
        {
            const auto& cell = screen.cells[row * WrdDisplayFrame::columns + column];
            line += cell.bytes.empty() ? " " : cell.bytes;
        }
    }
    return frame;
}

void applyCommand (const WrdToken& token,
                   TextScreen& screen,
                   std::vector<WrdDisplayFrame>& frames,
                   const wrdfile::Timing& timing,
                   WrdAssetStore& assets)
{
    const auto values = parseIntegers (token.arguments);
    bool changed = false;

    if (token.name == "@wait")
    {
        if (! values.empty())
            screen.bar = std::max (1, values[0]);
        screen.step = values.size() > 1 ? std::clamp (values[1], 0, wrdTicksPerMeasure - 1) : 0;
    }
    else if (token.name == "@rest")
    {
        screen.bar += values.empty() ? 1 : std::max (0, values[0]);
        screen.step = values.size() > 1 ? std::clamp (values[1], 0, wrdTicksPerMeasure - 1) : 0;
    }
    else if (token.name == "@inkey")
    {
        if (! values.empty())
            screen.bar = std::max (1, values[0]);
        screen.step = 0;
    }
    else if (token.name == "@offset")
    {
        screen.offsetBars = values.empty() ? 0 : values[0];
    }
    else if (token.name == "@wmode")
    {
        screen.deltaSteps = values.empty() ? 0 : values[0];
        screen.wmode = values.size() > 1 ? values[1] : 0;
        if (screen.wmode == 0)
            screen.step = 0;
    }
    else if (token.name == "@locate")
    {
        if (values.size() >= 2)
        {
            if (token.arguments.find (';') != std::string::npos)
                screen.locate (values[1], values[0]);
            else
                screen.locate (values[0], values[1]);
        }
    }
    else if (token.name == "@color")
    {
        screen.colour = values.empty() ? 23 : std::clamp (values[0], 0, 47);
    }
    else if (token.name == "@ton")
    {
        screen.textOn = values.empty() || values[0] != 0;
    }
    else if (token.name == "@gon")
    {
        screen.graphicsOn = values.empty() || values[0] != 0;
        screen.backgroundIndex = screen.graphicsOn
            ? screen.graphicsPages[static_cast<std::size_t> (screen.graphicsPage)]
            : -1;
        changed = true;
    }
    else if (token.name == "@gscreen")
    {
        if (! values.empty())
            screen.graphicsPage = std::clamp (values[0], 0, 1);
        screen.backgroundIndex = screen.graphicsOn
            ? screen.graphicsPages[static_cast<std::size_t> (screen.graphicsPage)]
            : -1;
        changed = true;
    }
    else if (token.name == "@mag")
    {
        const auto separator = token.arguments.find (',');
        const auto name = token.arguments.substr (0, separator);
        const auto imageIndex = assets.loadNamed (name);
        if (imageIndex >= 0)
        {
            screen.graphicsPages[static_cast<std::size_t> (screen.graphicsPage)] = imageIndex;
            if (screen.graphicsOn)
                screen.backgroundIndex = imageIndex;
            changed = true;
        }
    }
    else if (token.name == "@gcls")
    {
        screen.graphicsPages[static_cast<std::size_t> (screen.graphicsPage)] = -1;
        if (screen.graphicsOn)
            screen.backgroundIndex = -1;
        changed = true;
    }
    else if (token.name == "@tcls")
    {
        const auto firstColumn = values.size() > 0 ? values[0] : 1;
        const auto firstRow = values.size() > 1 ? values[1] : 1;
        const auto lastColumn = values.size() > 2 ? values[2]
                                                   : static_cast<int> (WrdDisplayFrame::columns);
        const auto lastRow = values.size() > 3 ? values[3]
                                                : static_cast<int> (WrdDisplayFrame::rows);
        const auto fillColour = values.size() > 4 ? std::clamp (values[4], 0, 47) : 23;
        screen.clearRange (firstColumn, firstRow, lastColumn, lastRow,
                           static_cast<std::uint8_t> (fillColour));
        changed = true;
    }
    else if (token.name == "@scroll")
    {
        screen.scroll (values);
        changed = true;
    }
    else if (token.name == "@esc")
    {
        screen.applyEscape (token.arguments);
        changed = true;
    }
    else if (token.name == "^tscrl")
    {
        screen.scroll ({ 1, 1, static_cast<int> (WrdDisplayFrame::columns),
                         static_cast<int> (WrdDisplayFrame::rows),
                         values.empty() ? 0 : values[0] });
        changed = true;
    }
    else if (token.name == "@end" || token.name == "@stop")
    {
        screen.textOn = false;
    }

    if (changed)
        frames.push_back (makeFrame (screen, timing));
}
}

namespace wrdfile
{
double Timing::secondsAtTick (const std::int64_t tick) const noexcept
{
    if (tick <= 0 || segments.empty())
        return 0.0;

    const auto it = std::upper_bound (segments.begin(), segments.end(), tick,
        [] (const std::int64_t value, const TempoSegment& segment)
        {
            return value < segment.startTick;
        });
    const auto& segment = it == segments.begin() ? segments.front() : *(it - 1);
    return segment.startSeconds
         + static_cast<double> (tick - segment.startTick) * 60.0
           / (std::max (1.0, segment.bpm) * std::max (1, timeBase));
}

bool loadForRcp (const std::string& rcpPath,
                 const Timing& timing,
                 MidiFileData& destination,
                 std::string& warning)
{
    destination.wrdFrames.clear();
    destination.wrdBackgrounds.clear();
    destination.wrdFilePath.clear();
    destination.wrdParseError.clear();
    warning.clear();

    if (rcpPath.empty())
        return true;

    const auto path = findCompanion (rcpPath);
    if (path.empty())
        return true;

    std::string contents;
    if (! readFileContents (path, contents))
    {
        warning = "WRD companion could not be read: " + path;
        destination.wrdParseError = warning;
        return false;
    }

    std::vector<WrdToken> tokens;
    if (! parseWrdTokens (contents, tokens, warning))
    {
        if (warning.empty())
            warning = "WRD companion is empty: " + path;
        destination.wrdParseError = warning;
        return false;
    }

    TextScreen screen;
    WrdAssetStore assets { path, destination, warning };

    // Some MIMPI WRDs use a same-basename MAG without an explicit @MAG
    // command.  Treat it as the initial graphics page; explicit WRD commands
    // can still replace or clear that page later.
    const auto defaultMag = findMagCompanion (rcpPath);
    if (! defaultMag.empty())
    {
        const auto imageIndex = assets.loadPath (defaultMag);
        if (imageIndex >= 0)
        {
            screen.graphicsPages[0] = imageIndex;
            screen.backgroundIndex = imageIndex;
        }
    }

    std::vector<WrdDisplayFrame> frames;
    frames.reserve (tokens.size());

    for (const auto& token : tokens)
    {
        if (token.command)
        {
            applyCommand (token, screen, frames, timing, assets);
            if (token.name == "@end" || token.name == "@stop")
                break;
        }
        else
        {
            screen.writeLyrics (token.text);
            frames.push_back (makeFrame (screen, timing));
        }

        if (! token.continuing)
            advanceWrdPosition (screen);
    }

    std::stable_sort (frames.begin(), frames.end(),
        [] (const WrdDisplayFrame& first, const WrdDisplayFrame& second)
        {
            return first.seconds < second.seconds;
        });

    std::vector<WrdDisplayFrame> compact;
    compact.reserve (frames.size());
    for (auto& frame : frames)
    {
        if (! std::isfinite (frame.seconds))
            continue;

        if (! compact.empty() && std::abs (compact.back().seconds - frame.seconds) < 1.0e-9)
            compact.back() = std::move (frame);
        else
            compact.push_back (std::move (frame));
    }

    destination.wrdFrames = std::move (compact);
    destination.wrdFilePath = path;
    if (! warning.empty())
        destination.wrdParseError = warning;
    return true;
}
}
