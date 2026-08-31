#include "WrdDisplayWindow.h"

#include "BinaryData.h"
#include "MidiFilePlayer.h"
#include "PluginProcessor.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#elif JUCE_WINDOWS
 #include <windows.h>
#endif

namespace
{
constexpr double wrdDisplayAspectRatio = 640.0 / 400.0;
constexpr float wrdLogicalWidth = 640.0f;
constexpr float wrdLogicalHeight = 400.0f;
constexpr float wrdCellWidth = wrdLogicalWidth / static_cast<float> (WrdDisplayFrame::columns);
constexpr float wrdCellHeight = wrdLogicalHeight / static_cast<float> (WrdDisplayFrame::rows);

juce::String decodeWrdText (const std::string& bytes)
{
    if (bytes.empty())
        return {};

#if JUCE_MAC
    if (auto* converted = CFStringCreateWithBytes (kCFAllocatorDefault,
                                                    reinterpret_cast<const UInt8*> (bytes.data()),
                                                    static_cast<CFIndex> (bytes.size()),
                                                    kCFStringEncodingShiftJIS, false))
    {
        const auto length = CFStringGetLength (converted);
        const auto maximumBytes = CFStringGetMaximumSizeForEncoding (length,
                                                                      kCFStringEncodingUTF8) + 1;
        std::string utf8 (static_cast<std::size_t> (std::max<CFIndex> (1, maximumBytes)), '\0');
        const auto success = CFStringGetCString (converted, utf8.data(),
                                                 static_cast<CFIndex> (utf8.size()),
                                                 kCFStringEncodingUTF8);
        CFRelease (converted);
        if (success)
            return juce::String::fromUTF8 (utf8.c_str());
    }
#elif JUCE_WINDOWS
    const auto byteCount = static_cast<int> (std::min<std::size_t> (
        bytes.size(), static_cast<std::size_t> (std::numeric_limits<int>::max())));
    const auto wideLength = MultiByteToWideChar (932, 0, bytes.data(), byteCount,
                                                 nullptr, 0);
    if (wideLength > 0)
    {
        std::vector<wchar_t> wide (static_cast<std::size_t> (wideLength) + 1, L'\0');
        if (MultiByteToWideChar (932, 0, bytes.data(), byteCount,
                                 wide.data(), wideLength) > 0)
        {
            return juce::String (juce::CharPointer_UTF16 (
                reinterpret_cast<const juce::CharPointer_UTF16::CharType*> (wide.data())),
                static_cast<std::size_t> (wideLength));
        }
    }
#endif

    // ASCII WRD files are common, and this is also a useful fallback for a
    // host that already supplied UTF-8 bytes.
    return juce::String::fromUTF8 (bytes.data(), static_cast<int> (bytes.size()));
}

juce::Colour colourForWrdValue (const std::uint8_t value)
{
    static const std::array<juce::Colour, 8> colours {
        juce::Colour (0xffb8b8b8), // default
        juce::Colour (0xffff5555), // red
        juce::Colour (0xff5599ff), // blue
        juce::Colour (0xffdf77ff), // purple
        juce::Colour (0xff66dd77), // green
        juce::Colour (0xff55dddd), // cyan
        juce::Colour (0xffffdd55), // yellow
        juce::Colour (0xffffffff)  // white
    };

    const auto normalised = value >= 30 && value <= 37
        ? static_cast<std::uint8_t> (value - 30)
        : value >= 17 && value <= 23
            ? static_cast<std::uint8_t> (value - 16)
            : static_cast<std::uint8_t> (value & 7u);
    return colours[std::min<std::size_t> (normalised, colours.size() - 1)];
}
}

class WrdDisplayWindow::WindowConstrainer final : public juce::ComponentBoundsConstrainer
{
public:
    explicit WindowConstrainer (const double contentAspectRatio) noexcept
        : contentAspectRatio (contentAspectRatio)
    {
    }

    bool setFrameSize (juce::BorderSize<int> newFrameSize) noexcept
    {
        if (newFrameSize == frameSize)
            return false;

        frameSize = newFrameSize;
        return true;
    }

    void checkBounds (juce::Rectangle<int>& bounds,
                      const juce::Rectangle<int>& previousBounds,
                      const juce::Rectangle<int>& limits,
                      const bool isStretchingTop,
                      const bool isStretchingLeft,
                      const bool isStretchingBottom,
                      const bool isStretchingRight) override
    {
        // The base constrainer applies the size and onscreen limits.  Its
        // aspect-ratio option cannot be used here because it constrains the
        // native window frame, while WRD's ratio belongs to the client area.
        juce::ComponentBoundsConstrainer::checkBounds (bounds, previousBounds, limits,
                                                        isStretchingTop, isStretchingLeft,
                                                        isStretchingBottom, isStretchingRight);

        const auto frameWidth = frameSize.getLeftAndRight();
        const auto frameHeight = frameSize.getTopAndBottom();
        const auto previousContentWidth = previousBounds.getWidth() - frameWidth;
        const auto previousContentHeight = previousBounds.getHeight() - frameHeight;
        const auto contentWidth = bounds.getWidth() - frameWidth;
        const auto contentHeight = bounds.getHeight() - frameHeight;

        if (contentWidth <= 0 || contentHeight <= 0
            || previousContentWidth <= 0 || previousContentHeight <= 0)
            return;

        const auto minimumContentWidth = std::max (1, getMinimumWidth() - frameWidth);
        const auto minimumContentHeight = std::max (1, getMinimumHeight() - frameHeight);
        const auto maximumContentWidth = std::max (minimumContentWidth,
                                                   getMaximumWidth() - frameWidth);
        const auto maximumContentHeight = std::max (minimumContentHeight,
                                                    getMaximumHeight() - frameHeight);

        const auto minimumScale = std::max (static_cast<double> (minimumContentHeight),
                                            static_cast<double> (minimumContentWidth)
                                                / contentAspectRatio);
        const auto maximumScale = std::min (static_cast<double> (maximumContentHeight),
                                            static_cast<double> (maximumContentWidth)
                                                / contentAspectRatio);

        const auto previousAspectRatio = static_cast<double> (previousContentWidth)
                                       / static_cast<double> (previousContentHeight);
        const auto requestedAspectRatio = static_cast<double> (contentWidth)
                                        / static_cast<double> (contentHeight);

        const bool adjustWidth = (isStretchingTop || isStretchingBottom)
                                   && ! (isStretchingLeft || isStretchingRight)
            ? true
            : (isStretchingLeft || isStretchingRight)
                && ! (isStretchingTop || isStretchingBottom)
                    ? false
                    : previousAspectRatio > requestedAspectRatio;

        const auto requestedScale = adjustWidth
            ? static_cast<double> (contentHeight)
            : static_cast<double> (contentWidth) / contentAspectRatio;
        const auto scale = juce::jlimit (minimumScale, maximumScale, requestedScale);
        const auto constrainedContentWidth = juce::roundToInt (scale * contentAspectRatio);
        const auto constrainedContentHeight = juce::roundToInt (scale);
        const auto constrainedWidth = constrainedContentWidth + frameWidth;
        const auto constrainedHeight = constrainedContentHeight + frameHeight;

        bounds.setSize (constrainedWidth, constrainedHeight);

        if ((isStretchingTop || isStretchingBottom) && ! (isStretchingLeft || isStretchingRight))
            bounds.setX (previousBounds.getX() + (previousBounds.getWidth() - bounds.getWidth()) / 2);
        else if ((isStretchingLeft || isStretchingRight) && ! (isStretchingTop || isStretchingBottom))
            bounds.setY (previousBounds.getY() + (previousBounds.getHeight() - bounds.getHeight()) / 2);
        else
        {
            if (isStretchingLeft)
                bounds.setX (previousBounds.getRight() - bounds.getWidth());

            if (isStretchingTop)
                bounds.setY (previousBounds.getBottom() - bounds.getHeight());
        }

        // The aspect adjustment can change the window edges after the base
        // class has performed its onscreen checks, so apply those checks once
        // more without enabling another aspect-ratio adjustment.
        juce::ComponentBoundsConstrainer::checkBounds (bounds, previousBounds, limits,
                                                        false, false, false, false);
    }

private:
    const double contentAspectRatio;
    juce::BorderSize<int> frameSize;
};

class WrdDisplayWindow::Content final : public juce::Component
{
public:
    Content()
        : typeface (juce::Typeface::createSystemTypefaceFor (
              BinaryData::DotGothic16Regular_ttf,
              static_cast<std::size_t> (BinaryData::DotGothic16Regular_ttfSize)))
    {
        setOpaque (true);
        rawLines.fill (" ");
        colours.fill (23);
    }

    void clear()
    {
        rawLines.fill ({});
        colours.fill (23);
        background = {};
        backgroundSource = nullptr;
        repaint();
    }

    void setFrame (const WrdDisplayFrame& frame, const MidiFileData& file)
    {
        const WrdBackgroundImage* nextBackground = nullptr;
        if (frame.backgroundIndex >= 0
            && static_cast<std::size_t> (frame.backgroundIndex) < file.wrdBackgrounds.size())
        {
            nextBackground = &file.wrdBackgrounds[static_cast<std::size_t> (frame.backgroundIndex)];
        }

        if (nextBackground != backgroundSource)
        {
            backgroundSource = nextBackground;
            background = nextBackground != nullptr ? makeImage (*nextBackground) : juce::Image();
        }

        for (std::size_t row = 0; row < WrdDisplayFrame::rows; ++row)
        {
            rawLines[row] = frame.lines[row];
            colours[row] = frame.colours[row];
        }
        repaint();
    }

    void paint (juce::Graphics& graphics) override
    {
        graphics.fillAll (juce::Colour (0xff080b10));

        const auto componentWidth = static_cast<float> (getWidth());
        const auto componentHeight = static_cast<float> (getHeight());
        if (componentWidth <= 0.0f || componentHeight <= 0.0f)
            return;

        // WRD is a PC-98 screen emulator, not a freely reflowing layout.  All
        // painting below uses the original 640x400 coordinate system.  The
        // only operation involving the host window size is this final affine
        // mapping, so the MAG and the text can never acquire different scales
        // or offsets.
        const auto componentAspectRatio = componentWidth / componentHeight;
        const auto screenWidth = componentAspectRatio > static_cast<float> (wrdDisplayAspectRatio)
            ? componentHeight * static_cast<float> (wrdDisplayAspectRatio)
            : componentWidth;
        const auto screenHeight = screenWidth / static_cast<float> (wrdDisplayAspectRatio);
        const auto screenX = (componentWidth - screenWidth) * 0.5f;
        const auto screenY = (componentHeight - screenHeight) * 0.5f;
        const auto transform = juce::AffineTransform::scale (screenWidth / wrdLogicalWidth,
                                                              screenHeight / wrdLogicalHeight)
                                  .translated (screenX, screenY);

        graphics.saveState();
        graphics.addTransform (transform);

        if (background.isValid())
        {
            graphics.drawImage (background,
                                0, 0,
                                juce::roundToInt (wrdLogicalWidth),
                                juce::roundToInt (wrdLogicalHeight),
                                0, 0, background.getWidth(), background.getHeight(),
                                false);
            // The source images are often bright PC-98 artwork.  A subtle
            // veil keeps the coloured lyric text legible without hiding the
            // MAG artwork.
            graphics.setColour (juce::Colour (0x55000000));
            graphics.fillRect (0, 0,
                               juce::roundToInt (wrdLogicalWidth),
                               juce::roundToInt (wrdLogicalHeight));
        }
        else
        {
            graphics.fillRect (0, 0,
                               juce::roundToInt (wrdLogicalWidth),
                               juce::roundToInt (wrdLogicalHeight));
        }

        // DotGothic16 is deliberately rendered in logical WRD units.  MIMPI's
        // fixed font cells are 8x16; rendering a whole Shift-JIS line as one
        // Unicode string would render the storage blank after each full-width
        // character as an additional visible advance.
        auto fontOptions = juce::FontOptions (typeface)
                               .withHeight (wrdCellHeight)
                               .withMetricsKind (juce::TypefaceMetricsKind::legacy);
        graphics.setFont (juce::Font (fontOptions));

        for (std::size_t row = 0; row < WrdDisplayFrame::rows; ++row)
        {
            graphics.setColour (colourForWrdValue (colours[row]));
            drawCellLine (graphics, rawLines[row], row);
        }

        graphics.restoreState();
    }

private:
    void drawCellLine (juce::Graphics& graphics,
                       const std::string& rawLine,
                       const std::size_t row) const
    {
        std::size_t byteIndex = 0;
        std::size_t column = 0;
        const auto y = juce::roundToInt (wrdCellHeight * static_cast<float> (row));

        while (byteIndex < rawLine.size() && column < WrdDisplayFrame::columns)
        {
            const auto firstByte = static_cast<unsigned char> (rawLine[byteIndex]);
            const auto isShiftJisLead = (firstByte >= 0x81 && firstByte <= 0x9f)
                                     || (firstByte >= 0xe0 && firstByte <= 0xfc);

            if (isShiftJisLead && byteIndex + 1 < rawLine.size()
                && column + 1 < WrdDisplayFrame::columns)
            {
                // The parser stores the second occupied MIMPI column as one
                // ASCII blank after the Shift-JIS pair.  It is a cell marker,
                // not another glyph to draw.
                if (firstByte != 0x81 || static_cast<unsigned char> (rawLine[byteIndex + 1]) != 0x40)
                {
                    const auto text = decodeWrdText (rawLine.substr (byteIndex, 2));
                    graphics.drawText (text,
                                       juce::roundToInt (wrdCellWidth * static_cast<float> (column)),
                                       y,
                                       juce::roundToInt (wrdCellWidth * 2.0f),
                                       juce::roundToInt (wrdCellHeight),
                                       juce::Justification::left, false);
                }
                byteIndex += 2;
                if (byteIndex < rawLine.size() && rawLine[byteIndex] == ' ')
                    ++byteIndex;
                column += 2;
            }
            else
            {
                if (firstByte != static_cast<unsigned char> (' '))
                {
                    const auto text = decodeWrdText (rawLine.substr (byteIndex, 1));
                    graphics.drawText (text,
                                       juce::roundToInt (wrdCellWidth * static_cast<float> (column)),
                                       y,
                                       juce::roundToInt (wrdCellWidth),
                                       juce::roundToInt (wrdCellHeight),
                                       juce::Justification::left, false);
                }
                ++byteIndex;
                ++column;
            }
        }
    }

    static juce::Image makeImage (const WrdBackgroundImage& source)
    {
        if (source.width <= 0 || source.height <= 0)
            return {};

        const auto requiredBytes = static_cast<std::size_t> (source.width)
                                  * static_cast<std::size_t> (source.height) * 3u;
        if (source.rgb.size() < requiredBytes)
            return {};

        juce::Image image (juce::Image::RGB, source.width, source.height, false);
        juce::Image::BitmapData bitmap (image, 0, 0, source.width, source.height,
                                        juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < source.height; ++y)
        {
            for (int x = 0; x < source.width; ++x)
            {
                const auto offset = (static_cast<std::size_t> (y)
                                   * static_cast<std::size_t> (source.width)
                                   + static_cast<std::size_t> (x)) * 3u;
                bitmap.setPixelColour (x, y,
                                       juce::Colour::fromRGB (source.rgb[offset + 0],
                                                              source.rgb[offset + 1],
                                                              source.rgb[offset + 2]));
            }
        }
        return image;
    }

    juce::Typeface::Ptr typeface;
    std::array<std::string, WrdDisplayFrame::rows> rawLines;
    std::array<std::uint8_t, WrdDisplayFrame::rows> colours;
    juce::Image background;
    const WrdBackgroundImage* backgroundSource = nullptr;
};

WrdDisplayWindow::WrdDisplayWindow (NukedSC55AudioProcessor& processorToUse)
    : juce::DocumentWindow ("WRD", juce::Colour (0xff080b10),
                            juce::DocumentWindow::allButtons),
      processor (processorToUse)
{
    windowConstrainer = std::make_unique<WindowConstrainer> (wrdDisplayAspectRatio);
    windowConstrainer->setSizeLimits (480, 300, 1440, 900);
    setConstrainer (windowConstrainer.get());

    content = new Content();
    setContentOwned (content, false);
    setResizable (true, true);
    setUsingNativeTitleBar (true);
    setSize (800, 500);
    startTimerHz (30);
}

WrdDisplayWindow::~WrdDisplayWindow()
{
    stopTimer();
    content = nullptr;
}

void WrdDisplayWindow::showForFile (const juce::String& fileName)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    userClosed = false;
    displayedFile = nullptr;
    displayedFrame = static_cast<std::size_t> (-1);
    setName ("WRD - " + fileName);
    setVisible (true);
    updateWindowFrameSize();
    toFront (true);
}

void WrdDisplayWindow::hideForPlaybackStop()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    userClosed = false;
    displayedFile = nullptr;
    displayedFrame = static_cast<std::size_t> (-1);
    if (content != nullptr)
        content->clear();
    setVisible (false);
}

void WrdDisplayWindow::timerCallback()
{
    if (userClosed)
        return;

    updateWindowFrameSize();

    const auto state = processor.getWrdDisplayState();
    if (! state.shouldBeVisible || state.file == nullptr || state.file->wrdFrames.empty())
    {
        if (isVisible())
            setVisible (false);
        displayedFile = nullptr;
        displayedFrame = static_cast<std::size_t> (-1);
        return;
    }

    const auto& frames = state.file->wrdFrames;
    const auto it = std::upper_bound (frames.begin(), frames.end(), state.positionSeconds,
        [] (const double position, const WrdDisplayFrame& frame)
        {
            return position < frame.seconds;
        });

    const auto frameIndex = it == frames.begin()
        ? static_cast<std::size_t> (-1)
        : static_cast<std::size_t> ((it - frames.begin()) - 1);

    if (displayedFile != state.file || displayedFrame != frameIndex)
    {
        displayedFile = state.file;
        displayedFrame = frameIndex;
        if (frameIndex == static_cast<std::size_t> (-1))
            content->clear();
        else
            content->setFrame (frames[frameIndex], *state.file);
    }
}

void WrdDisplayWindow::updateWindowFrameSize()
{
    if (windowConstrainer == nullptr)
        return;

    if (auto* peer = getPeer())
        if (const auto frameSize = peer->getFrameSizeIfPresent())
            if (windowConstrainer->setFrameSize (*frameSize))
                windowConstrainer->checkComponentBounds (this);
}

void WrdDisplayWindow::closeButtonPressed()
{
    userClosed = true;
    setVisible (false);
}
