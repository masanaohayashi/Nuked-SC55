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
#include "BinaryData.h"
#include "lcd.h"
//[/Headers]

#include "PluginEditor.h"


//[MiscUserDefs] You can add your own user definitions and misc code here...
class LcdDisplay final : public juce::Component,
                         private juce::Timer
{
public:
    LcdDisplay()
        : background (juce::Image::RGB, LCD_DISPLAY_WIDTH, LCD_DISPLAY_HEIGHT, false),
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
        g.fillAll (juce::Colour (0xffff6f0f));
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);

        const auto destination = getLocalBounds().toFloat();
        g.drawImage (background, destination,
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
            return;
        }

        juce::Image::BitmapData pixels (background,
                                       juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < LCD_DISPLAY_HEIGHT; ++y)
        {
            for (int x = 0; x < LCD_DISPLAY_WIDTH; ++x)
            {
                const auto offset = (y * LCD_DISPLAY_WIDTH + x) * 4;
                pixels.setPixelColour (x, y, juce::Colour::fromRGBA (
                    source[offset + 0], source[offset + 1],
                    source[offset + 2], source[offset + 3]));
            }
        }
    }

    void refreshDisplay()
    {
        displayEnabled = LCD_GetDisplayMask (displayMask.data(), LCD_DISPLAY_WIDTH);

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
    }

    juce::Image background;
    juce::Image glyphLayer;
    std::array<uint8_t, LCD_DISPLAY_WIDTH * LCD_DISPLAY_HEIGHT> displayMask {};
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

    lcd->setBounds (260, 36, 344, 124);

    acrylPanel.reset (new juce::Component());
    contentComponent.addAndMakeVisible (acrylPanel.get());
    acrylPanel->setName ("new component");

    acrylPanel->setBounds (640, 0, 102, 200);

    buttonLevelDec.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonLevelDec.get());
    buttonLevelDec->setButtonText (TRANS ("<"));
    buttonLevelDec->addListener (this);

    buttonLevelDec->setBounds (768, 70, 54, 20);

    buttonLevelDec2.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonLevelDec2.get());
    buttonLevelDec2->setButtonText (TRANS (">"));
    buttonLevelDec2->addListener (this);

    buttonLevelDec2->setBounds (822, 70, 54, 20);


    //[UserPreSize]
    //[/UserPreSize]

    setSize (1024, 200);


    //[Constructor] You can add your own custom stuff here..
    lcdDisplay.reset (new LcdDisplay());
    lcd->addAndMakeVisible (lcdDisplay.get());
    lcdDisplay->setBounds (lcd->getLocalBounds());
    audioProcessor.requestRomSelection();
    //[/Constructor]
}

NukedSC55AudioProcessorEditor::~NukedSC55AudioProcessorEditor()
{
    //[Destructor_pre]. You can add your own custom destruction code here..
    lcdDisplay = nullptr;
    //[/Destructor_pre]

    lcd = nullptr;
    acrylPanel = nullptr;
    buttonLevelDec = nullptr;
    buttonLevelDec2 = nullptr;


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

    //[UserPaint] Add your own custom painting code here..
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

void NukedSC55AudioProcessorEditor::buttonClicked (juce::Button* buttonThatWasClicked)
{
    //[UserbuttonClicked_Pre]
    //[/UserbuttonClicked_Pre]

    if (buttonThatWasClicked == buttonLevelDec.get())
    {
        //[UserButtonCode_buttonLevelDec] -- add your button handler code here..
        //[/UserButtonCode_buttonLevelDec]
    }
    else if (buttonThatWasClicked == buttonLevelDec2.get())
    {
        //[UserButtonCode_buttonLevelDec2] -- add your button handler code here..
        //[/UserButtonCode_buttonLevelDec2]
    }

    //[UserbuttonClicked_Post]
    //[/UserbuttonClicked_Post]
}



//[MiscUserCode] You can add your own definitions of your custom methods or any other code here...
//[/MiscUserCode]


//==============================================================================
#if 0
/*  -- Projucer information section --

    This is where the Projucer stores the metadata that describe this GUI layout, so
    make changes in here at your peril!

BEGIN_JUCER_METADATA

<JUCER_COMPONENT documentType="Component" className="NukedSC55AudioProcessorEditor"
                 componentName="" parentClasses="public juce::AudioProcessorEditor"
                 constructorParams="NukedSC55AudioProcessor&amp; p" variableInitialisers="AudioProcessorEditor (&amp;p), audioProcessor (p)"
                 scaleOnResize="1" scaleMode="keepAspect" snapPixels="8" snapActive="1"
                 snapShown="1" overlayOpacity="0.330" fixedSize="1" initialWidth="1024"
                 initialHeight="200">
  <BACKGROUND backgroundColour="ff323e44"/>
  <GENERICCOMPONENT name="" id="c4c91c74bed6da56" memberName="lcd" virtualName=""
                    explicitFocusOrder="0" pos="260 36 344 124" class="juce::Component"
                    params=""/>
  <GENERICCOMPONENT name="new component" id="a494044ed7202bfd" memberName="acrylPanel"
                    virtualName="" explicitFocusOrder="0" pos="640 0 102 200" class="juce::Component"
                    params=""/>
  <TEXTBUTTON name="" id="791b54d63a59beec" memberName="buttonLevelDec" virtualName=""
              explicitFocusOrder="0" pos="768 70 54 20" buttonText="&lt;" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="b0fabff2cff12ab0" memberName="buttonLevelDec2" virtualName=""
              explicitFocusOrder="0" pos="822 70 54 20" buttonText="&gt;" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
</JUCER_COMPONENT>

END_JUCER_METADATA
*/
#endif


//[EndFile] You can add extra defines here...
//[/EndFile]

