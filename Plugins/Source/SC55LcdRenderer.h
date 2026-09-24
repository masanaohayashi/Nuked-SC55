#pragma once

#include <juce_graphics/juce_graphics.h>

// Compose the LCD at its native resolution, then scale the complete image once.
class SC55LcdRenderer
{
public:
    void setFrame (const juce::Image& background, const juce::Image& glyphs,
                   bool displayEnabled)
    {
        frame = background.createCopy();
        if (frame.isNull() || ! displayEnabled || glyphs.isNull())
            return;

        juce::Graphics g (frame);
        g.drawImageAt (glyphs, 0, 0);
    }

    void paint (juce::Graphics& g, juce::Rectangle<float> destination)
    {
        if (frame.isNull() || destination.isEmpty())
            return;
        const juce::Graphics::ScopedSaveState state (g);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (frame, destination, juce::RectanglePlacement::stretchToFit, false);
    }

private:
    juce::Image frame;
};
