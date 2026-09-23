#include "../../Plugins/Source/SC55LcdRenderer.h"
#include "../../Plugins/Source/SC55Lcd.h"

#include <cmath>
#include <iostream>
#include "../../src/backend/lcd_font.h"
#include "../../src/backend/lcd_back.h"

static juce::Image render (SC55LcdRenderer& renderer, float scale)
{
    juce::Image output (juce::Image::RGB, juce::roundToInt (344 * scale),
                        juce::roundToInt (124 * scale), true, juce::SoftwareImageType());
    juce::Graphics g (output);
    g.addTransform (juce::AffineTransform::scale (scale));
    renderer.paint (g, { 0, 0, 344, 124 });
    return output;
}

static int blendedPixels (const juce::Image& image, juce::Rectangle<int> region)
{
    int count = 0;
    for (int y = region.getY(); y < region.getBottom(); ++y)
        for (int x = region.getX(); x < region.getRight(); ++x)
        {
            const auto value = image.getPixelAt (x, y).getRed();
            if (value > 0 && value < 255)
                ++count;
        }
    return count;
}

static bool samePixels (const juce::Image& a, const juce::Image& b, int tolerance = 0)
{
    if (a.getBounds() != b.getBounds())
        return false;
    for (int y = 0; y < a.getHeight(); ++y)
        for (int x = 0; x < a.getWidth(); ++x)
        {
            const auto ca = a.getPixelAt (x, y);
            const auto cb = b.getPixelAt (x, y);
            if (std::abs (int (ca.getRed()) - int (cb.getRed())) > tolerance
                || std::abs (int (ca.getGreen()) - int (cb.getGreen())) > tolerance
                || std::abs (int (ca.getBlue()) - int (cb.getBlue())) > tolerance)
                return false;
        }
    return true;
}

int main (int argc, char** argv)
{
    juce::Image background (juce::Image::RGB, LCD_DISPLAY_WIDTH, LCD_DISPLAY_HEIGHT,
                            true, juce::SoftwareImageType());
    background.clear (background.getBounds(), juce::Colours::white);
    juce::Image glyphs (juce::Image::ARGB, LCD_DISPLAY_WIDTH, LCD_DISPLAY_HEIGHT,
                        true, juce::SoftwareImageType());
    {
        juce::Graphics g (glyphs);
        g.setColour (juce::Colours::black);
        // All-on LCD fixture at the product's character and meter coordinates.
        for (int ch = 0; ch < 16; ++ch)
            for (int y = 0; y < 7; ++y)
                for (int x = 0; x < 5; ++x)
                    g.fillRect (153 + ch * 35 + x * 6, 11 + y * 6, 5, 5);
        for (int channel = 0; channel < 2; ++channel)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 16; ++x)
                    g.fillRect (293 + x * 26, 71 + channel * 88 + y * 11, 24, 9);
    }

    SC55LcdRenderer renderer;
    renderer.setFrame (background, glyphs, true);
    int failures = 0;
    // Each output pixel covers one complete six-pixel period. Its brightness
    // must be 255/6 for every phase, including when the gap misses all four
    // bilinear samples. This checks energy preservation, not merely grey edges.
    for (int phase = 0; phase < 6; ++phase)
    {
        juce::Image stripes (juce::Image::RGB, 36, 12, true, juce::SoftwareImageType());
        stripes.clear (stripes.getBounds(), juce::Colours::white);
        juce::Image dots (juce::Image::ARGB, 36, 12, true, juce::SoftwareImageType());
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 36; ++x)
                if ((x + phase) % 6 != 0)
                    dots.setPixelAt (x, y, juce::Colours::black);
        SC55LcdRenderer smallRenderer;
        smallRenderer.setFrame (stripes, dots, true);
        juce::Image reduced (juce::Image::RGB, 6, 2, true, juce::SoftwareImageType());
        { juce::Graphics g (reduced); smallRenderer.paint (g, { 0, 0, 6, 2 }); }
        bool pass = true;
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 6; ++x)
                pass &= std::abs (int (reduced.getPixelAt (x, y).getRed()) - 43) <= 3;
        std::cout << "six-pixel gap phase=" << phase << (pass ? " PASS\n" : " FAIL\n");
        failures += ! pass;
    }
    for (float scale : { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 1.0f })
    {
        const auto output = render (renderer, scale);
        const auto characters = juce::Rectangle<float> (72, 6, 255, 17)
                                    .transformedBy (juce::AffineTransform::scale (scale)).toNearestInt();
        const auto meters = juce::Rectangle<float> (137, 34, 187, 73)
                                .transformedBy (juce::AffineTransform::scale (scale)).toNearestInt();
        const auto textEdges = blendedPixels (output, characters);
        const auto meterEdges = blendedPixels (output, meters);
        // Nearest sampling loses subpixel gaps: the entire fixture stays binary.
        const bool pass = textEdges > 0 && meterEdges > 0;
        std::cout << "scale=" << scale << " character edge pixels=" << textEdges
                  << " meter edge pixels=" << meterEdges << (pass ? " PASS\n" : " FAIL\n");
        failures += ! pass;
        // Host scaling must agree with painting directly into physical bounds.
        // Also check that repeated paints do not change the frame.
        juce::Image expected (juce::Image::RGB, output.getWidth(), output.getHeight(),
                              true, juce::SoftwareImageType());
        SC55LcdRenderer directRenderer;
        directRenderer.setFrame (background, glyphs, true);
        { juce::Graphics g (expected); directRenderer.paint (g, expected.getBounds().toFloat()); }
        // Equivalent fractional transforms may round coverage by 1-2/255.
        if (! samePixels (output, expected, 2) || ! samePixels (output, render (renderer, scale)))
        {
            int different = 0;
            int maxError = 0;
            for (int y = 0; y < output.getHeight(); ++y)
                for (int x = 0; x < output.getWidth(); ++x)
                {
                    const auto error = std::abs (int (output.getPixelAt (x, y).getRed())
                                                - int (expected.getPixelAt (x, y).getRed()));
                    different += error != 0;
                    maxError = juce::jmax (maxError, error);
                }
            std::cout << "transform/repaint mismatch FAIL pixels=" << different
                      << " maxError=" << maxError << '\n';
            ++failures;
        }

    }

    // The display-off/ROM state must discard the previous glyph frame.
    background.clear (background.getBounds(), juce::Colour (0xff707070));
    renderer.setFrame (background, glyphs, false);
    const auto off = render (renderer, 1.0f);
    for (int y = 0; y < off.getHeight(); ++y)
        for (int x = 0; x < off.getWidth(); ++x)
            if (off.getPixelAt (x, y) != juce::Colour (0xff707070))
                ++failures;

    // Updating content at the same size must replace the rectangle lists.
    background.clear (background.getBounds(), juce::Colours::white);
    renderer.setFrame (background, glyphs, true);
    const auto onAgain = render (renderer, 1.0f);
    SC55LcdRenderer freshRenderer;
    freshRenderer.setFrame (background, glyphs, true);
    if (! samePixels (onAgain, render (freshRenderer, 1.0f)))
        ++failures;
    glyphs.clear (glyphs.getBounds());
    renderer.setFrame (background, glyphs, true);
    const auto cleared = render (renderer, 1.0f);
    if (! samePixels (cleared, background.rescaled (344, 124)))
        ++failures;

    if (argc > 1)
    {
        // A ROM-free Piano 1 display preview using the real background/font.
        juce::Image glass (juce::Image::RGB, LCD_DISPLAY_WIDTH, LCD_DISPLAY_HEIGHT,
                            false, juce::SoftwareImageType());
        for (int y = 0; y < LCD_DISPLAY_HEIGHT; ++y)
            for (int x = 0; x < LCD_DISPLAY_WIDTH; ++x)
                glass.setPixelAt (x, y, back_data[y * LCD_DISPLAY_WIDTH + x] == 0
                    ? juce::Colour (0xffff6f0f) : juce::Colours::black);
        juce::Image dots (juce::Image::ARGB, LCD_DISPLAY_WIDTH, LCD_DISPLAY_HEIGHT,
                          true, juce::SoftwareImageType());
        {
            juce::Graphics g (dots);
            const auto text = [&](int x, int y, const char* str)
            {
                for (int ch = 0; str[ch] != 0; ++ch)
                    for (int row = 0; row < 7; ++row)
                        for (int col = 0; col < 5; ++col)
                        {
                            const bool lit = (lcd_font[static_cast<unsigned char> (str[ch]) - 16][row]
                                               & (1 << (4 - col))) != 0;
                            g.setColour (lit ? juce::Colours::black : juce::Colour (0xffc85000));
                            g.fillRect (x + ch * 35 + col * 6, y + row * 6, 5, 5);
                        }
            };
            text (34, 11, "  1"); text (153, 11, " 1 Piano 1      ");
            text (34, 75, "100"); text (153, 75, "  0");
            text (34, 139, " 40"); text (153, 139, "  0");
            text (34, 203, "  0"); text (153, 203, "  1");
            g.setColour (juce::Colour (0xffc85000));
            for (int channel = 0; channel < 2; ++channel)
                for (int row = 0; row < 8; ++row)
                    for (int col = 0; col < 16; ++col)
                        g.fillRect (293 + col * 26, 71 + channel * 88 + row * 11, 24, 9);
        }
        SC55LcdRenderer preview;
        preview.setFrame (glass, dots, true);
        juce::Image result (juce::Image::RGB, 244, 88, true, juce::SoftwareImageType());
        { juce::Graphics g (result); preview.paint (g, result.getBounds().toFloat()); }
        juce::File file (juce::String::fromUTF8 (argv[1]));
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat().writeImageToStream (result, *stream);
    }

    std::cout << "failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
