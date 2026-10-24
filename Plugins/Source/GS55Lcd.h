#pragma once
// The SC-55's LCD, drawn in a 741 x 268 frame (the private SysEx panel image
// uses the same frame): the part and instrument fields across the top, the
// six value fields below with their labels, and the 16-part level meter.
// Text fields are dot-matrix character cells (r2juce::R2CharacterLcd).

#include <JuceHeader.h>
#include "GS55Engine.h"
#include <array>
#include <functional>
#include <memory>
#include <vector>

class GS55Lcd final : public juce::Component, private juce::Timer
{
public:
    static constexpr int width = 741, height = 268;

    struct Source
    {
        std::function<bool (GS55Engine::LcdFrame&)> frame;                      // false: nothing to show
        std::function<bool (uint8_t*, size_t, bool*, bool)> panelImage;         // SysEx image, when shown
        std::function<bool()> romLoaded;
        std::function<void()> refreshed;                                         // after each update
    };

    explicit GS55Lcd (Source sourceToUse) : source (std::move (sourceToUse))
    {
        setOpaque (true);
        const std::array<int, 8> lengths { 3, 16, 3, 3, 3, 3, 3, 3 };
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            auto& field = fields[i];
            field = std::make_unique<r2juce::R2CharacterLcd>();
            r2juce::R2CharacterLcd::Layout layout;
            layout.columns = lengths[i];
            layout.rows = 1;
            layout.dotWidth = layout.dotHeight = dot;
            layout.dotGapX = layout.dotGapY = dotGap;
            layout.characterGap = characterGap;
            layout.lineGap = 0;
            layout.padding = {};
            field->setLayout (layout);
            field->setPalette ({ litColour, unlitColour, juce::Colours::transparentBlack });
            field->setScaling (r2juce::R2CharacterLcd::Scaling::fit);
            field->loadStandardCharacterSet();
            field->setInterceptsMouseClicks (false, false);
            addAndMakeVisible (*field);
        }
        panelPixels.resize (static_cast<size_t> (width * height));
        refresh();
        startTimerHz (30);
    }

    ~GS55Lcd() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        const auto frameArea = frameBounds();
        g.addTransform (juce::AffineTransform::scale (frameArea.getWidth() / width, frameArea.getHeight() / height)
                            .translated (frameArea.getX(), frameArea.getY()));
        g.setColour (romLoaded ? backgroundColour : noRomColour);
        g.fillRect (0, 0, width, height);
        if (showingPanelImage)
        {
            g.drawImageAt (panelImage, 0, 0);
            return;
        }
        paintLabels (g);
        paintMeter (g);
    }

    void resized() override
    {
        const auto area = frameBounds();
        const auto scale = area.getWidth() / width;
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            const auto& place = fieldPlaces[i];
            const auto columns = i == 1 ? 16 : 3;
            const auto w = columns * glyphInk (5, dot, dotGap) + (columns - 1) * characterGap;
            const auto h = glyphInk (8, dot, dotGap);
            fields[i]->setBounds (juce::Rectangle<float> (area.getX() + place.x * scale, area.getY() + place.y * scale,
                                                          w * scale, h * scale).getSmallestIntegerContainer());
        }
    }

private:
    struct Place { float x, y; };
    static constexpr float dot = 5, dotGap = 1, characterGap = 6;
    static float glyphInk (int dots, float size, float gap) { return dots * size + (dots - 1) * gap; }

    // Fields: part, instrument, level, pan, reverb, chorus, key shift, MIDI channel.
    static constexpr std::array<Place, 8> fieldPlaces {{
        { 34, 11 }, { 153, 11 }, { 34, 75 }, { 153, 75 }, { 34, 139 }, { 153, 139 }, { 34, 203 }, { 153, 203 } }};
    // Meter: 16 part columns, 16 cells high (two blocks of eight).
    static constexpr float meterLeft = 293, meterColumn = 26, cellWidth = 24, cellHeight = 9, cellPitch = 11;
    static constexpr std::array<float, 2> meterBlockTop { 71, 159 };

    const juce::Colour backgroundColour { 0xffff6f0f }, noRomColour { 0xff707070 };
    const juce::Colour litColour { 0xff000000 }, unlitColour { 0xffc85000 };

    juce::Rectangle<float> frameBounds() const
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto scale = std::min (bounds.getWidth() / width, bounds.getHeight() / height);
        return juce::Rectangle<float> (width * scale, height * scale).withCentre (bounds.getCentre());
    }

    void paintLabels (juce::Graphics& g) const
    {
        g.setColour (litColour);
        g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        const auto label = [&] (const char* text, float x, float y)
        { g.drawText (text, juce::Rectangle<float> (x, y, 110, 16), juce::Justification::centredLeft, false); };
        label ("LEVEL", 36, 56);   label ("PAN", 155, 56);
        label ("REVERB", 36, 120); label ("CHORUS", 155, 120);
        label ("K SHIFT", 36, 184); label ("MIDI CH", 155, 184);

        // The meter's scale: eleven dots, larger at the ends and the middle.
        for (int i = 0; i <= 10; ++i)
        {
            const float y = 73.0f + i * 16.4f, r = (i % 5 == 0) ? 3.0f : 1.6f;
            g.fillEllipse (287.0f - r, y - r, 2 * r, 2 * r);
        }
        g.setFont (juce::Font (juce::FontOptions (15.0f)));
        for (int part = 0; part < 16; ++part)
            g.drawText (juce::String (part + 1),
                        juce::Rectangle<float> (meterLeft + part * meterColumn - 2, 244, cellWidth + 4, 16),
                        juce::Justification::centred, false);
    }

    void paintMeter (juce::Graphics& g) const
    {
        juce::RectangleList<float> lit, unlit;
        for (int part = 0; part < 16; ++part)
            for (int block = 0; block < 2; ++block)
                for (int row = 0; row < 8; ++row)
                {
                    const juce::Rectangle<float> cell (meterLeft + part * meterColumn,
                                                       meterBlockTop[(size_t) block] + row * cellPitch, cellWidth, cellHeight);
                    (meterCellLit (part, block, row) ? lit : unlit).addWithoutMerging (cell);
                }
        g.setColour (unlitColour); g.fillRectList (unlit);
        g.setColour (litColour); g.fillRectList (lit);
    }

    bool meterCellLit (int part, int block, int row) const
    {
        if (! frame.enabled)
            return false;
        if (frame.meterFromGlyphs)
        {
            // A SysEx meter bitmap: glyph pairs (upper, lower) for each five
            // parts, one glyph column per part.
            const auto& glyph = frame.glyphs[(size_t) ((part / 5) * 2 + block)];
            return (glyph[(size_t) row] >> (4 - part % 5)) & 1;
        }
        const auto threshold = unsigned (block == 0 ? 16 - row : 8 - row);
        return frame.meter[(size_t) part] >= threshold;
    }

    void timerCallback() override { refresh(); }

    void refresh()
    {
        const bool loaded = source.romLoaded && source.romLoaded();
        bool changed = loaded != romLoaded;
        romLoaded = loaded;

        bool imageChanged = false;
        const bool image = source.panelImage
            && source.panelImage (panelPixels.data(), width, &imageChanged, ! showingPanelImage);
        if (image && (imageChanged || ! showingPanelImage))
            rebuildPanelImage();
        changed |= image != showingPanelImage || (image && imageChanged);
        showingPanelImage = image;

        GS55Engine::LcdFrame next;
        const bool hasFrame = ! image && source.frame && source.frame (next);
        if (! hasFrame)
            next = {};
        changed |= showFrame (next);
        for (auto& field : fields)
            field->setVisible (! image);
        if (changed)
            repaint();
        if (source.refreshed)
            source.refreshed();
    }

    bool showFrame (const GS55Engine::LcdFrame& next)
    {
        bool meterChanged = next.meter != frame.meter || next.meterFromGlyphs != frame.meterFromGlyphs
                         || next.enabled != frame.enabled || (next.meterFromGlyphs && next.glyphs != frame.glyphs);
        frame = next;
        const std::array<const uint8_t*, 8> text { frame.part.data(), frame.instrument.data(), frame.level.data(),
            frame.pan.data(), frame.reverb.data(), frame.chorus.data(), frame.keyShift.data(), frame.midiChannel.data() };
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            auto& field = *fields[i];
            field.setDisplayEnabled (frame.enabled);
            // Codes 0-7 (and their mirrors 8-15) are the eight user glyphs.
            for (int code = 0; code < 16; ++code)
                field.registerCharacterRows ((char32_t) code, frame.glyphs[(size_t) (code & 7)]);
            const int columns = i == 1 ? 16 : 3;
            for (int c = 0; c < columns; ++c)
                field.setCell (c, 0, (char32_t) text[i][c]);
        }
        return meterChanged;
    }

    void rebuildPanelImage()
    {
        if (! panelImage.isValid())
            panelImage = juce::Image (juce::Image::ARGB, width, height, true);
        juce::Image::BitmapData pixels (panelImage, juce::Image::BitmapData::writeOnly);
        const juce::Colour palette[4] { juce::Colours::transparentBlack, litColour, unlitColour, juce::Colours::white };
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                pixels.setPixelColour (x, y, palette[panelPixels[(size_t) (y * width + x)] & 3]);
    }

    Source source;
    std::array<std::unique_ptr<r2juce::R2CharacterLcd>, 8> fields;
    GS55Engine::LcdFrame frame;
    bool romLoaded = false, showingPanelImage = false;
    std::vector<uint8_t> panelPixels;
    juce::Image panelImage;
};
