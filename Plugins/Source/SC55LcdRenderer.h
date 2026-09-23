#pragma once

#include <juce_graphics/juce_graphics.h>
#include <vector>

// Message-thread LCD presentation. Like TX81Z's CharacterLcdComponent,
// draw lit/unlit dots as floating-point rectangles, not a scaled glyph bitmap.
class SC55LcdRenderer
{
public:
    void setFrame (const juce::Image& background, const juce::Image& glyphs,
                   bool displayEnabled)
    {
        backgroundImage = background;
        on.clear();
        off.clear();
        if (! displayEnabled || glyphs.isNull())
            return;

        // Recover solid rectangles from the existing emulator mask. Keep each
        // dot/segment whole: independently drawing its individual pixel rows
        // would introduce internal antialiased edges at fractional scales.
        const juce::Image::BitmapData pixels (glyphs, juce::Image::BitmapData::readOnly);
        const int width = glyphs.getWidth(), height = glyphs.getHeight();
        std::vector<bool> used (static_cast<size_t> (width) * height, false);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const auto colour = pixels.getPixelColour (x, y);
                if (colour.isTransparent() || used[static_cast<size_t> (y) * width + x])
                    continue;
                const auto matches = [&](int px, int py)
                {
                    return ! used[static_cast<size_t> (py) * width + px]
                        && pixels.getPixelColour (px, py) == colour;
                };
                int right = x + 1;
                while (right < width && matches (right, y))
                    ++right;
                int bottom = y + 1;
                while (bottom < height)
                {
                    int px = x;
                    while (px < right && matches (px, bottom))
                        ++px;
                    if (px != right)
                        break;
                    ++bottom;
                }
                for (int py = y; py < bottom; ++py)
                    for (int px = x; px < right; ++px)
                        used[static_cast<size_t> (py) * width + px] = true;
                (colour == juce::Colours::black ? on : off).addWithoutMerging (
                    { float (x), float (y), float (right - x), float (bottom - y) });
            }
    }

    void paint (juce::Graphics& g, juce::Rectangle<float> destination)
    {
        if (backgroundImage.isNull() || destination.isEmpty())
            return;
        const juce::Graphics::ScopedSaveState state (g);
        g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
        g.drawImage (backgroundImage, destination, juce::RectanglePlacement::stretchToFit, false);
        g.addTransform (juce::AffineTransform::scale (
            destination.getWidth() / backgroundImage.getWidth(),
            destination.getHeight() / backgroundImage.getHeight())
                .translated (destination.getX(), destination.getY()));
        g.setColour (juce::Colour (0xffc85000));
        g.fillRectList (off);
        g.setColour (juce::Colours::black);
        g.fillRectList (on);
    }

private:
    juce::Image backgroundImage;
    juce::RectangleList<float> on, off;
};
