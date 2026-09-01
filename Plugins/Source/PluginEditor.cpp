/*
  ==============================================================================

  This is an automatically generated GUI class created by the Projucer!

  Be careful when adding custom code to these files, as only the code within
  the "//[xyz]" and "//[/xyz]" sections will be retained when the file is loaded
  and re-saved.

  Created with Projucer version: 9.0.0

  ------------------------------------------------------------------------------

  The Projucer is part of the JUCE library.
  Copyright (c) - Raw Material Software Limited.

  ==============================================================================
*/

//[Headers] You can add your own extra header files here...
#include <array>
#include <functional>
#include <utility>
#include "BinaryData.h"
#include "SC55Lcd.h"
//[/Headers]

#include "PluginEditor.h"


//[MiscUserDefs] You can add your own user definitions and misc code here...
class LcdDisplay final : public juce::Component,
                         private juce::Timer
{
public:
    explicit LcdDisplay (NukedSC55AudioProcessor& processorToUse,
                         std::function<void()> refreshCallbackToUse = {})
        : processor (processorToUse),
          refreshCallback (std::move (refreshCallbackToUse)),
          background (juce::Image::RGB, LCD_DISPLAY_WIDTH, LCD_DISPLAY_HEIGHT, false),
          noRomBackground (juce::Image::RGB, LCD_DISPLAY_WIDTH, LCD_DISPLAY_HEIGHT, false),
          glyphLayer (juce::Image::ARGB, LCD_DISPLAY_WIDTH, LCD_DISPLAY_HEIGHT, true)
    {
        setOpaque (true);
        loadBackground();
        refreshDisplay();
        startTimerHz (30);
    }

    ~LcdDisplay() override
    {
        stopTimer();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (romLoaded ? juce::Colour (0xffff6f0f)
                            : juce::Colour (0xff707070));
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);

        const auto destination = getLocalBounds().toFloat();
        g.drawImage (romLoaded ? background : noRomBackground, destination,
                     juce::RectanglePlacement::stretchToFit, false);

        if (displayEnabled)
            g.drawImage (glyphLayer, destination,
                         juce::RectanglePlacement::stretchToFit, false);
    }

private:
    void timerCallback() override
    {
        refreshDisplay();
    }

    void loadBackground()
    {
        const auto* source = reinterpret_cast<const uint8_t*> (BinaryData::back_data);
        if (source == nullptr
            || BinaryData::back_dataSize < LCD_DISPLAY_WIDTH * LCD_DISPLAY_HEIGHT * 4)
        {
            background.clear (background.getBounds(), juce::Colour (0xffff6f0f));
            noRomBackground.clear (noRomBackground.getBounds(), juce::Colour (0xff707070));
            return;
        }

        juce::Image::BitmapData pixels (background,
                                       juce::Image::BitmapData::writeOnly);
        juce::Image::BitmapData noRomPixels (noRomBackground,
                                            juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < LCD_DISPLAY_HEIGHT; ++y)
        {
            for (int x = 0; x < LCD_DISPLAY_WIDTH; ++x)
            {
                const auto offset = (y * LCD_DISPLAY_WIDTH + x) * 4;
                const auto colour = juce::Colour::fromRGBA (
                    source[offset + 0], source[offset + 1],
                    source[offset + 2], source[offset + 3]);
                pixels.setPixelColour (x, y, colour);
                noRomPixels.setPixelColour (x, y,
                                            colour == juce::Colour (0xffff6f0f)
                                                ? juce::Colour (0xff707070)
                                                : colour);
            }
        }
    }

    void refreshDisplay()
    {
        romLoaded = processor.getUiStatus().audioReady;
        displayEnabled = romLoaded
                      && processor.copyLcdDisplay (displayMask.data(), LCD_DISPLAY_WIDTH);

        juce::Image::BitmapData pixels (glyphLayer,
                                       juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < LCD_DISPLAY_HEIGHT; ++y)
        {
            for (int x = 0; x < LCD_DISPLAY_WIDTH; ++x)
            {
                const auto value = displayMask[static_cast<size_t> (y)
                                               * LCD_DISPLAY_WIDTH + x];
                const auto colour = value == 1
                                  ? juce::Colours::black
                                  : value == 2
                                    ? juce::Colour (0xffc85000)
                                    : juce::Colours::transparentBlack;
                pixels.setPixelColour (x, y, colour);
            }
        }

        repaint();
        if (refreshCallback)
            refreshCallback();
    }

    NukedSC55AudioProcessor& processor;
    std::function<void()> refreshCallback;
    juce::Image background;
    juce::Image noRomBackground;
    juce::Image glyphLayer;
    std::array<uint8_t, LCD_DISPLAY_WIDTH * LCD_DISPLAY_HEIGHT> displayMask {};
    bool romLoaded = false;
    bool displayEnabled = false;
};
//[/MiscUserDefs]

//==============================================================================
NukedSC55AudioProcessorEditor::NukedSC55AudioProcessorEditor (NukedSC55AudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    //[Constructor_pre] You can add your own custom stuff here..
    //[/Constructor_pre]

    addAndMakeVisible (contentComponent);
    lcd.reset (new juce::Component());
    contentComponent.addAndMakeVisible (lcd.get());

    lcd->setBounds (260, 38, 344, 124);

    sliderMasterVolume.reset (new juce::Slider (juce::String()));
    contentComponent.addAndMakeVisible (sliderMasterVolume.get());
    sliderMasterVolume->setRange (0, 100, 1);
    sliderMasterVolume->setSliderStyle (juce::Slider::RotaryVerticalDrag);
    sliderMasterVolume->setTextBoxStyle (juce::Slider::NoTextBox, false, 80, 20);
    sliderMasterVolume->addListener (this);
    filmstripSliderLookAndFeel1.setFilmstrip (juce::ImageCache::getFromMemory (BinaryData::Volume_png, BinaryData::Volume_pngSize), 101, true);
    sliderMasterVolume->setLookAndFeel (&filmstripSliderLookAndFeel1);

    sliderMasterVolume->setBounds (132, 24, 64, 64);

    label2x.reset (new juce::Label (juce::String(),
                                    TRANS ("2X")));
    contentComponent.addAndMakeVisible (label2x.get());
    label2x->setFont (juce::Font (juce::FontOptions { 15.00f, juce::Font::plain }.withStyle ("Regular").withMetricsKind (juce::TypefaceMetricsKind::legacy)));
    label2x->setJustificationType (juce::Justification::centredRight);
    label2x->setEditable (false, false, false);
    label2x->setColour (juce::Label::textColourId, juce::Colour (0x80ffffff));
    label2x->setColour (juce::TextEditor::textColourId, juce::Colours::black);
    label2x->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0x00000000));

    label2x->setBounds (648, 112, 46, 16);

    buttonPlayPause.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonPlayPause.get());
    buttonPlayPause->setButtonText (TRANS ("PLAY"));
    buttonPlayPause->addListener (this);

    buttonPlayPause->setBounds (88, 144, 64, 24);

    buttonStop.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonStop.get());
    buttonStop->setButtonText (TRANS ("STOP"));
    buttonStop->addListener (this);

    buttonStop->setBounds (16, 144, 64, 24);

    buttonPartDec2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonPartDec2.get());
    buttonPartDec2->setButtonText (TRANS ("new button"));
    buttonPartDec2->addListener (this);

    buttonPartDec2->setImages (false, true, true,
                               juce::ImageCache::getFromMemory (BinaryData::PartDecButton_normal_png, BinaryData::PartDecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::PartDecButton_over_png, BinaryData::PartDecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::PartDecButton_down_png, BinaryData::PartDecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonPartDec2->setBounds (768, 24, 52, 20);

    buttonPartInc2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonPartInc2.get());
    buttonPartInc2->addListener (this);

    buttonPartInc2->setImages (false, true, true,
                               juce::ImageCache::getFromMemory (BinaryData::PartIncButton_normal_png, BinaryData::PartIncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::PartIncButton_over_png, BinaryData::PartIncButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::PartIncButton_down_png, BinaryData::PartIncButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonPartInc2->setBounds (820, 24, 52, 20);

    buttonInstDec2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonInstDec2.get());
    buttonInstDec2->setButtonText (TRANS ("new button"));
    buttonInstDec2->addListener (this);

    buttonInstDec2->setImages (false, true, true,
                               juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonInstDec2->setBounds (894, 24, 52, 20);

    buttonInstInc2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonInstInc2.get());
    buttonInstInc2->addListener (this);

    buttonInstInc2->setImages (false, true, true,
                               juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonInstInc2->setBounds (946, 24, 52, 20);

    ledPower.reset (new r2juce::R2Led (juce::ImageCache::getFromMemory (BinaryData::Led_png, BinaryData::Led_pngSize), 11));
    contentComponent.addAndMakeVisible (ledPower.get());

    ledPower->setBounds (106, 30, 8, 8);

    buttonMakerLogo.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonMakerLogo.get());
    buttonMakerLogo->addListener (this);

    buttonMakerLogo->setImages (false, true, true,
                                juce::ImageCache::getFromMemory (BinaryData::MakerLogo_png, BinaryData::MakerLogo_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::Image(), 1.000f, juce::Colour (0x00000000),
                                juce::Image(), 1.000f, juce::Colour (0x00000000));
    buttonMakerLogo->setBounds (504, 8, 103, 24);

    buttonSC.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonSC.get());
    buttonSC->addListener (this);

    buttonSC->setImages (false, true, true,
                         juce::ImageCache::getFromMemory (BinaryData::SC_png, BinaryData::SC_pngSize), 1.000f, juce::Colour (0x00000000),
                         juce::Image(), 1.000f, juce::Colour (0x00000000),
                         juce::Image(), 1.000f, juce::Colour (0x00000000));
    buttonSC->setBounds (251, 166, 192, 24);

    buttonMk2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonMk2.get());
    buttonMk2->addListener (this);

    buttonMk2->setImages (false, true, true,
                          juce::ImageCache::getFromMemory (BinaryData::mk2_png, BinaryData::mk2_pngSize), 1.000f, juce::Colour (0x00000000),
                          juce::Image(), 1.000f, juce::Colour (0x00000000),
                          juce::Image(), 1.000f, juce::Colour (0x00000000));
    buttonMk2->setBounds (484, 169, 124, 18);

    buttonAll_new.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonAll_new.get());
    buttonAll_new->setButtonText (TRANS ("new button"));
    buttonAll_new->addListener (this);

    buttonAll_new->setImages (false, true, true,
                              juce::ImageCache::getFromMemory (BinaryData::LedButton_off_png, BinaryData::LedButton_off_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::LedButton_off_png, BinaryData::LedButton_off_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::LedButton_on_png, BinaryData::LedButton_on_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonAll_new->setBounds (696, 22, 24, 24);

    buttonMute_new.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonMute_new.get());
    buttonMute_new->setButtonText (TRANS ("new button"));
    buttonMute_new->addListener (this);

    buttonMute_new->setImages (false, true, true,
                               juce::ImageCache::getFromMemory (BinaryData::LedButton_off_png, BinaryData::LedButton_off_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::LedButton_off_png, BinaryData::LedButton_off_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::LedButton_on_png, BinaryData::LedButton_on_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonMute_new->setBounds (696, 64, 24, 24);

    button2x_new.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (button2x_new.get());
    button2x_new->addListener (this);

    button2x_new->setImages (false, true, true,
                             juce::ImageCache::getFromMemory (BinaryData::LedButton_off_png, BinaryData::LedButton_off_pngSize), 1.000f, juce::Colour (0x00000000),
                             juce::ImageCache::getFromMemory (BinaryData::LedButton_off_png, BinaryData::LedButton_off_pngSize), 1.000f, juce::Colour (0x00000000),
                             juce::ImageCache::getFromMemory (BinaryData::LedButton_on_png, BinaryData::LedButton_on_pngSize), 1.000f, juce::Colour (0x00000000));
    button2x_new->setBounds (696, 108, 24, 24);

    buttonPower2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonPower2.get());
    buttonPower2->addListener (this);

    buttonPower2->setImages (false, true, true,
                             juce::ImageCache::getFromMemory (BinaryData::PowerButton_normal_png, BinaryData::PowerButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                             juce::ImageCache::getFromMemory (BinaryData::PowerButton_over_png, BinaryData::PowerButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                             juce::ImageCache::getFromMemory (BinaryData::PowerButton_down_png, BinaryData::PowerButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonPower2->setBounds (24, 24, 72, 20);

    buttonLevelDec2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonLevelDec2.get());
    buttonLevelDec2->setButtonText (TRANS ("new button"));
    buttonLevelDec2->addListener (this);

    buttonLevelDec2->setImages (false, true, true,
                                juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonLevelDec2->setBounds (768, 67, 52, 20);

    buttonLevelInc2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonLevelInc2.get());
    buttonLevelInc2->addListener (this);

    buttonLevelInc2->setImages (false, true, true,
                                juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonLevelInc2->setBounds (820, 67, 52, 20);

    buttonPanDec2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonPanDec2.get());
    buttonPanDec2->setButtonText (TRANS ("new button"));
    buttonPanDec2->addListener (this);

    buttonPanDec2->setImages (false, true, true,
                              juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonPanDec2->setBounds (894, 67, 52, 20);

    buttonPanInc2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonPanInc2.get());
    buttonPanInc2->addListener (this);

    buttonPanInc2->setImages (false, true, true,
                              juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonPanInc2->setBounds (946, 67, 52, 20);

    buttonReverbDec2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonReverbDec2.get());
    buttonReverbDec2->setButtonText (TRANS ("new button"));
    buttonReverbDec2->addListener (this);

    buttonReverbDec2->setImages (false, true, true,
                                 juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                 juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                                 juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonReverbDec2->setBounds (768, 110, 52, 20);

    buttonReverbInc2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonReverbInc2.get());
    buttonReverbInc2->addListener (this);

    buttonReverbInc2->setImages (false, true, true,
                                 juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                 juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                                 juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonReverbInc2->setBounds (820, 110, 52, 20);

    buttonChorusDec2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonChorusDec2.get());
    buttonChorusDec2->setButtonText (TRANS ("new button"));
    buttonChorusDec2->addListener (this);

    buttonChorusDec2->setImages (false, true, true,
                                 juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                 juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                                 juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonChorusDec2->setBounds (894, 110, 52, 20);

    buttonChorusInc2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonChorusInc2.get());
    buttonChorusInc2->addListener (this);

    buttonChorusInc2->setImages (false, true, true,
                                 juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                 juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                                 juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonChorusInc2->setBounds (946, 110, 52, 20);

    buttonKeyShiftDec2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonKeyShiftDec2.get());
    buttonKeyShiftDec2->setButtonText (TRANS ("new button"));
    buttonKeyShiftDec2->addListener (this);

    buttonKeyShiftDec2->setImages (false, true, true,
                                   juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                   juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                                   juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonKeyShiftDec2->setBounds (768, 154, 52, 20);

    buttonKeyShiftInc2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonKeyShiftInc2.get());
    buttonKeyShiftInc2->addListener (this);

    buttonKeyShiftInc2->setImages (false, true, true,
                                   juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                   juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                                   juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonKeyShiftInc2->setBounds (820, 154, 52, 20);

    buttonMidiChDec2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonMidiChDec2.get());
    buttonMidiChDec2->setButtonText (TRANS ("new button"));
    buttonMidiChDec2->addListener (this);

    buttonMidiChDec2->setImages (false, true, true,
                                 juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                 juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                                 juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonMidiChDec2->setBounds (894, 154, 52, 20);

    buttonMidiChInc2.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonMidiChInc2.get());
    buttonMidiChInc2->addListener (this);

    buttonMidiChInc2->setImages (false, true, true,
                                 juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                 juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                                 juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonMidiChInc2->setBounds (946, 154, 52, 20);

    buttonLoad.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonLoad.get());
    buttonLoad->setButtonText (TRANS ("LOAD"));
    buttonLoad->addListener (this);

    buttonLoad->setBounds (160, 144, 64, 24);

    labelPlayer.reset (new juce::Label (juce::String(),
                                        TRANS ("RCP/MID PLAYER")));
    contentComponent.addAndMakeVisible (labelPlayer.get());
    labelPlayer->setFont (juce::Font (juce::FontOptions { 15.00f, juce::Font::plain }.withStyle ("Regular").withMetricsKind (juce::TypefaceMetricsKind::legacy)));
    labelPlayer->setJustificationType (juce::Justification::centredLeft);
    labelPlayer->setEditable (false, false, false);
    labelPlayer->setColour (juce::Label::textColourId, juce::Colour (0x80ffffff));
    labelPlayer->setColour (juce::TextEditor::textColourId, juce::Colours::black);
    labelPlayer->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0x00000000));

    labelPlayer->setBounds (16, 120, 144, 16);

    cachedImage_BinaryData_Background_png_2 = juce::ImageCache::getFromMemory (BinaryData::Background_png, BinaryData::Background_pngSize);

    //[UserPreSize]
    //[/UserPreSize]

    setSize (1024, 200);


    //[Constructor] You can add your own custom stuff here..
    // The faceplate is rendered at its native 1024x200 size and scaled in
    // resized().  Keep the host window locked to the faceplate's aspect ratio,
    // as on the TX81Z reference editor, so a resize never leaves a stretched
    // panel surrounded by large empty margins.
    setResizable (true, false);
    setResizeLimits (512, 100, 2048, 400);
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio (1024.0 / 200.0);
    lcdDisplay.reset (new LcdDisplay (audioProcessor,
                                      [this] { syncFrontPanelIndicators(); }));
    lcd->addAndMakeVisible (lcdDisplay.get());
    lcdDisplay->setBounds (lcd->getLocalBounds());
    masterVolumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        audioProcessor.getParameters(), "masterVolume", *sliderMasterVolume);
    audioProcessor.requestRomSelection();
    button2x_new->setClickingTogglesState (true);
    button2x_new->setToggleState (audioProcessor.isTwoXEnabled(), juce::dontSendNotification);
    syncFrontPanelIndicators();
    //[/Constructor]
}

NukedSC55AudioProcessorEditor::~NukedSC55AudioProcessorEditor()
{
    //[Destructor_pre]. You can add your own custom destruction code here..
    masterVolumeAttachment = nullptr;
    lcdDisplay = nullptr;
    //[/Destructor_pre]

    lcd = nullptr;
    sliderMasterVolume->setLookAndFeel (nullptr);
    sliderMasterVolume = nullptr;
    label2x = nullptr;
    buttonPlayPause = nullptr;
    buttonStop = nullptr;
    buttonPartDec2 = nullptr;
    buttonPartInc2 = nullptr;
    buttonInstDec2 = nullptr;
    buttonInstInc2 = nullptr;
    ledPower = nullptr;
    buttonMakerLogo = nullptr;
    buttonSC = nullptr;
    buttonMk2 = nullptr;
    buttonAll_new = nullptr;
    buttonMute_new = nullptr;
    button2x_new = nullptr;
    buttonPower2 = nullptr;
    buttonLevelDec2 = nullptr;
    buttonLevelInc2 = nullptr;
    buttonPanDec2 = nullptr;
    buttonPanInc2 = nullptr;
    buttonReverbDec2 = nullptr;
    buttonReverbInc2 = nullptr;
    buttonChorusDec2 = nullptr;
    buttonChorusInc2 = nullptr;
    buttonKeyShiftDec2 = nullptr;
    buttonKeyShiftInc2 = nullptr;
    buttonMidiChDec2 = nullptr;
    buttonMidiChInc2 = nullptr;
    buttonLoad = nullptr;
    labelPlayer = nullptr;


    //[Destructor]. You can add your own custom destruction code here..
    //[/Destructor]
}

//==============================================================================
void NukedSC55AudioProcessorEditor::paint (juce::Graphics& g)
{
    //[UserPrePaint] Add your own custom painting code here..
    //[/UserPrePaint]

    juce::Graphics::ScopedSaveState scaledContentState (g);
    auto scaleX = getWidth() / 1024.0f;
    auto scaleY = getHeight() / 200.0f;
    auto scale = juce::jmin (scaleX, scaleY);
    g.addTransform (juce::AffineTransform::scale (scale)
                        .translated ((getWidth() - 1024 * scale) * 0.5f,
                                     (getHeight() - 200 * scale) * 0.5f));

    g.fillAll (juce::Colour (0xff323e44));

    {
        int x = 0, y = 0, width = 1024, height = 200;
        //[UserPaintCustomArguments] Customize the painting arguments here..
        //[/UserPaintCustomArguments]
        g.setColour (juce::Colours::black);
        g.drawImageWithin (cachedImage_BinaryData_Background_png_2,
                           x, y, width, height,
                           juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize,
                           false);
    }

    //[UserPaint] Add your own custom painting code here..
    if (fileDragActive)
    {
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.fillAll();
        g.setColour (juce::Colours::white);
        g.drawRect (getLocalBounds(), 3);
    }
    //[/UserPaint]
}

void NukedSC55AudioProcessorEditor::resized()
{
    //[UserPreResize] Add your own custom resize code here..
    //[/UserPreResize]

    contentComponent.setBounds (0, 0, 1024, 200);
    auto scaleX = getWidth() / 1024.0f;
    auto scaleY = getHeight() / 200.0f;
    auto scale = juce::jmin (scaleX, scaleY);
    contentComponent.setTransform (juce::AffineTransform::scale (scale)
                                       .translated ((getWidth() - 1024 * scale) * 0.5f,
                                                    (getHeight() - 200 * scale) * 0.5f));

    //[UserResized] Add your own custom resize handling here..
    if (lcdDisplay != nullptr && lcd != nullptr)
        lcdDisplay->setBounds (lcd->getLocalBounds());
    //[/UserResized]
}

void NukedSC55AudioProcessorEditor::sliderValueChanged (juce::Slider* sliderThatWasMoved)
{
    //[UsersliderValueChanged_Pre]
    //[/UsersliderValueChanged_Pre]

    if (sliderThatWasMoved == sliderMasterVolume.get())
    {
        //[UserSliderCode_sliderMasterVolume] -- add your slider handling code here..
        //[/UserSliderCode_sliderMasterVolume]
    }

    //[UsersliderValueChanged_Post]
    //[/UsersliderValueChanged_Post]
}

void NukedSC55AudioProcessorEditor::buttonClicked (juce::Button* buttonThatWasClicked)
{
    //[UserbuttonClicked_Pre]
    //[/UserbuttonClicked_Pre]

    if (buttonThatWasClicked == buttonPlayPause.get())
    {
        //[UserButtonCode_buttonPlayPause] -- add your button handler code here..
        if (audioProcessor.isPlayingMidiFile())
            audioProcessor.pauseMidiFile();
        else
            audioProcessor.playMidiFile();

        syncPlaybackControls();
        //[/UserButtonCode_buttonPlayPause]
    }
    else if (buttonThatWasClicked == buttonStop.get())
    {
        //[UserButtonCode_buttonStop] -- add your button handler code here..
        audioProcessor.stopMidiFile();
        syncPlaybackControls();
        //[/UserButtonCode_buttonStop]
    }
    else if (buttonThatWasClicked == buttonPartDec2.get())
    {
        //[UserButtonCode_buttonPartDec2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::partDec);
        //[/UserButtonCode_buttonPartDec2]
    }
    else if (buttonThatWasClicked == buttonPartInc2.get())
    {
        //[UserButtonCode_buttonPartInc2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::partInc);
        //[/UserButtonCode_buttonPartInc2]
    }
    else if (buttonThatWasClicked == buttonInstDec2.get())
    {
        //[UserButtonCode_buttonInstDec2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::instrumentDec);
        //[/UserButtonCode_buttonInstDec2]
    }
    else if (buttonThatWasClicked == buttonInstInc2.get())
    {
        //[UserButtonCode_buttonInstInc2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::instrumentInc);
        //[/UserButtonCode_buttonInstInc2]
    }
    else if (buttonThatWasClicked == buttonMakerLogo.get())
    {
        //[UserButtonCode_buttonMakerLogo] -- add your button handler code here..
        //[/UserButtonCode_buttonMakerLogo]
    }
    else if (buttonThatWasClicked == buttonSC.get())
    {
        //[UserButtonCode_buttonSC] -- add your button handler code here..
        //[/UserButtonCode_buttonSC]
    }
    else if (buttonThatWasClicked == buttonMk2.get())
    {
        //[UserButtonCode_buttonMk2] -- add your button handler code here..
        //[/UserButtonCode_buttonMk2]
    }
    else if (buttonThatWasClicked == buttonAll_new.get())
    {
        //[UserButtonCode_buttonAll_new] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::all);
        syncFrontPanelIndicators();
        //[/UserButtonCode_buttonAll_new]
    }
    else if (buttonThatWasClicked == buttonMute_new.get())
    {
        //[UserButtonCode_buttonMute_new] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::mute);
        syncFrontPanelIndicators();
        //[/UserButtonCode_buttonMute_new]
    }
    else if (buttonThatWasClicked == button2x_new.get())
    {
        //[UserButtonCode_button2x_new] -- add your button handler code here..
        audioProcessor.setTwoXEnabled (button2x_new->getToggleState());
        syncFrontPanelIndicators();
        //[/UserButtonCode_button2x_new]
    }
    else if (buttonThatWasClicked == buttonPower2.get())
    {
        //[UserButtonCode_buttonPower2] -- add your button handler code here..
        audioProcessor.requestGsReset();
        //[/UserButtonCode_buttonPower2]
    }
    else if (buttonThatWasClicked == buttonLevelDec2.get())
    {
        //[UserButtonCode_buttonLevelDec2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::levelDec);
        //[/UserButtonCode_buttonLevelDec2]
    }
    else if (buttonThatWasClicked == buttonLevelInc2.get())
    {
        //[UserButtonCode_buttonLevelInc2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::levelInc);
        //[/UserButtonCode_buttonLevelInc2]
    }
    else if (buttonThatWasClicked == buttonPanDec2.get())
    {
        //[UserButtonCode_buttonPanDec2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::panDec);
        //[/UserButtonCode_buttonPanDec2]
    }
    else if (buttonThatWasClicked == buttonPanInc2.get())
    {
        //[UserButtonCode_buttonPanInc2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::panInc);
        //[/UserButtonCode_buttonPanInc2]
    }
    else if (buttonThatWasClicked == buttonReverbDec2.get())
    {
        //[UserButtonCode_buttonReverbDec2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::reverbDec);
        //[/UserButtonCode_buttonReverbDec2]
    }
    else if (buttonThatWasClicked == buttonReverbInc2.get())
    {
        //[UserButtonCode_buttonReverbInc2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::reverbInc);
        //[/UserButtonCode_buttonReverbInc2]
    }
    else if (buttonThatWasClicked == buttonChorusDec2.get())
    {
        //[UserButtonCode_buttonChorusDec2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::chorusDec);
        //[/UserButtonCode_buttonChorusDec2]
    }
    else if (buttonThatWasClicked == buttonChorusInc2.get())
    {
        //[UserButtonCode_buttonChorusInc2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::chorusInc);
        //[/UserButtonCode_buttonChorusInc2]
    }
    else if (buttonThatWasClicked == buttonKeyShiftDec2.get())
    {
        //[UserButtonCode_buttonKeyShiftDec2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::keyShiftDec);
        //[/UserButtonCode_buttonKeyShiftDec2]
    }
    else if (buttonThatWasClicked == buttonKeyShiftInc2.get())
    {
        //[UserButtonCode_buttonKeyShiftInc2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::keyShiftInc);
        //[/UserButtonCode_buttonKeyShiftInc2]
    }
    else if (buttonThatWasClicked == buttonMidiChDec2.get())
    {
        //[UserButtonCode_buttonMidiChDec2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::midiChannelDec);
        //[/UserButtonCode_buttonMidiChDec2]
    }
    else if (buttonThatWasClicked == buttonMidiChInc2.get())
    {
        //[UserButtonCode_buttonMidiChInc2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::midiChannelInc);
        //[/UserButtonCode_buttonMidiChInc2]
    }
    else if (buttonThatWasClicked == buttonLoad.get())
    {
        //[UserButtonCode_buttonLoad] -- add your button handler code here..
        showSequenceFileChooser();
        //[/UserButtonCode_buttonLoad]
    }

    //[UserbuttonClicked_Post]
    //[/UserbuttonClicked_Post]
}



//[MiscUserCode] You can add your own definitions of your custom methods or any other code here...

void NukedSC55AudioProcessorEditor::loadSequenceFile (const juce::File& file)
{
    if (file == juce::File {})
        return;

    if (! audioProcessor.loadMidiFile (file))
    {
        const auto options = juce::MessageBoxOptions::makeOptionsOk (
            juce::AlertWindow::WarningIcon, "SC-55",
            "このシーケンスファイルを再生できませんでした:\n" + file.getFileName());
        juce::AlertWindow::showAsync (options, nullptr);
    }

    syncPlaybackControls();
}

void NukedSC55AudioProcessorEditor::showSequenceFileChooser()
{
    sequenceFileChooser = std::make_unique<juce::FileChooser> (
        "Load MIDI/RCP file",
        juce::File::getCurrentWorkingDirectory(),
        "*.mid;*.midi;*.smf;*.rcp",
        true);

    const auto chooserFlags = juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles;
    const juce::Component::SafePointer<NukedSC55AudioProcessorEditor> safeThis (this);
    sequenceFileChooser->launchAsync (chooserFlags,
                                      [safeThis] (const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr)
            return;

        const auto file = chooser.getResult();
        if (file == juce::File {})
        {
            safeThis->sequenceFileChooser = nullptr;
            return;
        }

        safeThis->loadSequenceFile (file);
        safeThis->sequenceFileChooser = nullptr;
    });
}

void NukedSC55AudioProcessorEditor::syncFrontPanelIndicators()
{
    const auto uiStatus = audioProcessor.getUiStatus();
    const auto& state = uiStatus.emulator;
    const auto syncIndicatorState = [] (juce::ImageButton* button, bool isLit)
    {
        if (button == nullptr)
            return;

        button->setToggleState (isLit, juce::dontSendNotification);
    };

    syncIndicatorState (buttonAll_new.get(), state.allLed);
    syncIndicatorState (buttonMute_new.get(), state.muteLed);
    if (ledPower != nullptr)
        ledPower->setValue (uiStatus.audioReady ? 1.0f : 0.0f);
    const auto twoXEnabled = audioProcessor.isTwoXEnabled();
    if (button2x_new != nullptr)
        button2x_new->setToggleState (twoXEnabled, juce::dontSendNotification);
    syncPlaybackControls();
}

void NukedSC55AudioProcessorEditor::syncPlaybackControls()
{
    const auto hasFile = audioProcessor.hasMidiFile();
    const auto isPlaying = hasFile && audioProcessor.isPlayingMidiFile();

    if (buttonPlayPause != nullptr)
    {
        buttonPlayPause->setEnabled (hasFile);
        const juce::String desiredText = isPlaying ? "PAUSE" : "PLAY";
        if (buttonPlayPause->getButtonText() != desiredText)
            buttonPlayPause->setButtonText (desiredText);
    }

    if (buttonStop != nullptr)
        buttonStop->setEnabled (hasFile);
}

//==============================================================================
// Drop a Standard MIDI File or RCP sequence on the window to load it. Press
// PLAY to start the resulting event stream in file order; no sequencer sits in
// between to sort or de-duplicate controllers.
bool NukedSC55AudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase (".mid") || f.endsWithIgnoreCase (".midi")
            || f.endsWithIgnoreCase (".smf") || f.endsWithIgnoreCase (".rcp"))
            return true;

    return false;
}

void NukedSC55AudioProcessorEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    fileDragActive = true;
    repaint();
}

void NukedSC55AudioProcessorEditor::fileDragExit (const juce::StringArray&)
{
    fileDragActive = false;
    repaint();
}

void NukedSC55AudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    fileDragActive = false;
    repaint();

    for (const auto& f : files)
    {
        const juce::File file (f);
        if (! isInterestedInFileDrag ({ f }))
            continue;

        loadSequenceFile (file);

        return;
    }
}

//[/MiscUserCode]


//==============================================================================
#if 0
/*  -- Projucer information section --

    This is where the Projucer stores the metadata that describe this GUI layout, so
    make changes in here at your peril!

BEGIN_JUCER_METADATA

<JUCER_COMPONENT documentType="Component" className="NukedSC55AudioProcessorEditor"
                 componentName="" parentClasses="public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget"
                 constructorParams="NukedSC55AudioProcessor&amp; p" variableInitialisers="AudioProcessorEditor (&amp;p), audioProcessor (p)"
                 scaleOnResize="1" scaleMode="keepAspect" snapPixels="8" snapActive="1"
                 snapShown="1" overlayOpacity="0.330" fixedSize="1" initialWidth="1024"
                 initialHeight="200">
  <BACKGROUND backgroundColour="ff323e44">
    <IMAGE pos="0 0 1024 200" resource="BinaryData::Background_png" opacity="1.0"
           mode="2"/>
  </BACKGROUND>
  <GENERICCOMPONENT name="" id="c4c91c74bed6da56" memberName="lcd" virtualName=""
                    explicitFocusOrder="0" pos="260 38 344 124" class="juce::Component"
                    params=""/>
  <SLIDER name="" id="56f744301daef134" memberName="sliderMasterVolume"
          virtualName="" explicitFocusOrder="0" pos="132 24 64 64" min="0.0"
          max="100.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="1" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1" filmstripImage="BinaryData::Volume_png" filmstripFrames="101"
          filmstripVertical="1"/>
  <LABEL name="" id="571536871ed7a09d" memberName="label2x" virtualName=""
         explicitFocusOrder="0" pos="648 112 46 16" textCol="80ffffff"
         edTextCol="ff000000" edBkgCol="0" labelText="2X" editableSingleClick="0"
         editableDoubleClick="0" focusDiscardsChanges="0" fontname="Default font"
         fontsize="15.0" kerning="0.0" bold="0" italic="0" justification="34"/>
  <TEXTBUTTON name="" id="d38aac467e703aaf" memberName="buttonPlayPause" virtualName=""
              explicitFocusOrder="0" pos="88 144 64 24" buttonText="PLAY" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="ae0fc65259da31ea" memberName="buttonStop" virtualName=""
              explicitFocusOrder="0" pos="16 144 64 24" buttonText="STOP" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <IMAGEBUTTON name="" id="bd7a7d1e8ecefc56" memberName="buttonPartDec2" virtualName=""
               explicitFocusOrder="0" pos="768 24 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::PartDecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::PartDecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::PartDecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="e5595339a02d2464" memberName="buttonPartInc2" virtualName=""
               explicitFocusOrder="0" pos="820 24 52 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::PartIncButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::PartIncButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::PartIncButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="3c18f144f8b62f68" memberName="buttonInstDec2" virtualName=""
               explicitFocusOrder="0" pos="894 24 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="5bdf0a672aeba4a0" memberName="buttonInstInc2" virtualName=""
               explicitFocusOrder="0" pos="946 24 52 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::IncButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::IncButton_down_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::IncButton_over_png"
               opacityDown="1.0" colourDown="0"/>
  <GENERICCOMPONENT name="" id="aac2d7e83f6cf7b" memberName="ledPower" virtualName="r2juce::R2Led"
                    explicitFocusOrder="0" pos="106 30 8 8" class="juce::Component"
                    params="juce::ImageCache::getFromMemory (BinaryData::Led_png, BinaryData::Led_pngSize), 11"/>
  <IMAGEBUTTON name="" id="3f55e20683436ef8" memberName="buttonMakerLogo" virtualName=""
               explicitFocusOrder="0" pos="504 8 103 24" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::MakerLogo_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="" opacityOver="1.0"
               colourOver="0" resourceDown="" opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="4beeb0441e53d280" memberName="buttonSC" virtualName=""
               explicitFocusOrder="0" pos="251 166 192 24" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::SC_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="" opacityOver="1.0"
               colourOver="0" resourceDown="" opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="b7f4c2171b20f4e1" memberName="buttonMk2" virtualName=""
               explicitFocusOrder="0" pos="484 169 124 18" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::mk2_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="" opacityOver="1.0"
               colourOver="0" resourceDown="" opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="7e618566428e4ed6" memberName="buttonAll_new" virtualName=""
               explicitFocusOrder="0" pos="696 22 24 24" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::LedButton_off_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::LedButton_off_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::LedButton_on_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="40fe940c238ae3ca" memberName="buttonMute_new" virtualName=""
               explicitFocusOrder="0" pos="696 64 24 24" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::LedButton_off_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::LedButton_off_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::LedButton_on_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="a87ba9651dd9c626" memberName="button2x_new" virtualName=""
               explicitFocusOrder="0" pos="696 108 24 24" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::LedButton_off_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::LedButton_off_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::LedButton_on_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="44ae4a19ee6707ae" memberName="buttonPower2" virtualName=""
               explicitFocusOrder="0" pos="24 24 72 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::PowerButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::PowerButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::PowerButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="c3b257319e397d96" memberName="buttonLevelDec2" virtualName=""
               explicitFocusOrder="0" pos="768 67 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="4806a33c53deff13" memberName="buttonLevelInc2" virtualName=""
               explicitFocusOrder="0" pos="820 67 52 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::IncButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::IncButton_down_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::IncButton_over_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="d89f1d7b86b80be3" memberName="buttonPanDec2" virtualName=""
               explicitFocusOrder="0" pos="894 67 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="a0cb50c25e14cf63" memberName="buttonPanInc2" virtualName=""
               explicitFocusOrder="0" pos="946 67 52 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::IncButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::IncButton_down_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::IncButton_over_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="743751e5ee71c550" memberName="buttonReverbDec2" virtualName=""
               explicitFocusOrder="0" pos="768 110 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="f896f520f9905fa8" memberName="buttonReverbInc2" virtualName=""
               explicitFocusOrder="0" pos="820 110 52 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::IncButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::IncButton_down_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::IncButton_over_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="bfa466bbf4b0a117" memberName="buttonChorusDec2" virtualName=""
               explicitFocusOrder="0" pos="894 110 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="58ddc8d78d0c69e4" memberName="buttonChorusInc2" virtualName=""
               explicitFocusOrder="0" pos="946 110 52 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::IncButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::IncButton_down_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::IncButton_over_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="4a67d7a98526be72" memberName="buttonKeyShiftDec2"
               virtualName="" explicitFocusOrder="0" pos="768 154 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="a7905cd531c3d999" memberName="buttonKeyShiftInc2"
               virtualName="" explicitFocusOrder="0" pos="820 154 52 20" buttonText=""
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::IncButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::IncButton_down_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::IncButton_over_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="4b095cd63fef7324" memberName="buttonMidiChDec2" virtualName=""
               explicitFocusOrder="0" pos="894 154 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="5892b2fed7947741" memberName="buttonMidiChInc2" virtualName=""
               explicitFocusOrder="0" pos="946 154 52 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::IncButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::IncButton_down_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::IncButton_over_png"
               opacityDown="1.0" colourDown="0"/>
  <TEXTBUTTON name="" id="fc55a74cb8cec1ed" memberName="buttonLoad" virtualName=""
              explicitFocusOrder="0" pos="160 144 64 24" buttonText="LOAD"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <LABEL name="" id="461158e76b2fd0d6" memberName="labelPlayer" virtualName=""
         explicitFocusOrder="0" pos="16 120 144 16" textCol="80ffffff"
         edTextCol="ff000000" edBkgCol="0" labelText="RCP/MID PLAYER"
         editableSingleClick="0" editableDoubleClick="0" focusDiscardsChanges="0"
         fontname="Default font" fontsize="15.0" kerning="0.0" bold="0"
         italic="0" justification="33"/>
</JUCER_COMPONENT>

END_JUCER_METADATA
*/
#endif


//[EndFile] You can add extra defines here...
//[/EndFile]

