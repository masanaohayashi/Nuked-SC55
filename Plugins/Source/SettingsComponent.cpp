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
//[/Headers]

#include "SettingsComponent.h"


//[MiscUserDefs] You can add your own user definitions and misc code here...
//[/MiscUserDefs]

//==============================================================================
SettingsComponent::SettingsComponent ()
{
    //[Constructor_pre] You can add your own custom stuff here..
    //[/Constructor_pre]

    addAndMakeVisible (contentComponent);
    buttonClose.reset (new juce::ImageButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonClose.get());
    buttonClose->addListener (this);

    buttonClose->setImages (false, true, true,
                            juce::ImageCache::getFromMemory (BinaryData::PowerButton_normal_png, BinaryData::PowerButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                            juce::ImageCache::getFromMemory (BinaryData::PowerButton_over_png, BinaryData::PowerButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                            juce::ImageCache::getFromMemory (BinaryData::PowerButton_down_png, BinaryData::PowerButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonClose->setBounds (24, 24, 72, 20);

    buttonAudioDevice.reset (new juce::TextButton ("new button"));
    contentComponent.addAndMakeVisible (buttonAudioDevice.get());
    buttonAudioDevice->setButtonText (TRANS ("Audio Device Settings"));
    buttonAudioDevice->addListener (this);

    buttonAudioDevice->setBounds (24, 80, 272, 24);

    labelCurrentRom.reset (new juce::Label (juce::String(),
                                            TRANS ("n/a")));
    contentComponent.addAndMakeVisible (labelCurrentRom.get());
    labelCurrentRom->setFont (juce::Font (juce::FontOptions { 15.00f, juce::Font::plain }.withStyle ("Regular").withMetricsKind (juce::TypefaceMetricsKind::legacy)));
    labelCurrentRom->setJustificationType (juce::Justification::centredLeft);
    labelCurrentRom->setEditable (false, false, false);
    labelCurrentRom->setColour (juce::Label::backgroundColourId, juce::Colours::black);
    labelCurrentRom->setColour (juce::TextEditor::textColourId, juce::Colours::black);
    labelCurrentRom->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0x00000000));

    labelCurrentRom->setBounds (344, 48, 208, 24);

    labelCurrentRomCaption.reset (new juce::Label (juce::String(),
                                                   TRANS ("ROM")));
    contentComponent.addAndMakeVisible (labelCurrentRomCaption.get());
    labelCurrentRomCaption->setFont (juce::Font (juce::FontOptions { 15.00f, juce::Font::plain }.withStyle ("Regular").withMetricsKind (juce::TypefaceMetricsKind::legacy)));
    labelCurrentRomCaption->setJustificationType (juce::Justification::centredLeft);
    labelCurrentRomCaption->setEditable (false, false, false);
    labelCurrentRomCaption->setColour (juce::TextEditor::textColourId, juce::Colours::black);
    labelCurrentRomCaption->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0x00000000));

    labelCurrentRomCaption->setBounds (344, 16, 150, 24);

    buttonLoadRom.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonLoadRom.get());
    buttonLoadRom->setButtonText (TRANS ("Load ROM"));
    buttonLoadRom->addListener (this);

    buttonLoadRom->setBounds (344, 80, 208, 24);


    //[UserPreSize]
    //[/UserPreSize]

    setSize (1024, 200);


    //[Constructor] You can add your own custom stuff here..
    buttonGsReset = std::make_unique<juce::TextButton> ("GS");
    contentComponent.addAndMakeVisible (buttonGsReset.get());
    buttonGsReset->setButtonText (TRANS ("GS"));
    buttonGsReset->addListener (this);
    buttonGsReset->setBounds (600, 80, 80, 24);

    buttonGmReset = std::make_unique<juce::TextButton> ("GM");
    contentComponent.addAndMakeVisible (buttonGmReset.get());
    buttonGmReset->setButtonText (TRANS ("GM"));
    buttonGmReset->addListener (this);
    buttonGmReset->setBounds (696, 80, 80, 24);

#if JUCE_STANDALONE_APPLICATION
    setAudioDeviceButtonEnabled (true);
#else
    setAudioDeviceButtonEnabled (false);
#endif

    labelCurrentRom->setColour (juce::Label::textColourId, juce::Colours::white);
    labelCurrentRomCaption->setColour (juce::Label::textColourId,
                                       juce::Colours::white.withAlpha (0.7f));
#if JUCE_IOS
    buttonLoadRom->setButtonText (TRANS ("Load ROM Folder"));
#endif
    //[/Constructor]
}

SettingsComponent::~SettingsComponent()
{
    //[Destructor_pre]. You can add your own custom destruction code here..
    //[/Destructor_pre]

    buttonClose = nullptr;
    buttonAudioDevice = nullptr;
    labelCurrentRom = nullptr;
    labelCurrentRomCaption = nullptr;
    buttonLoadRom = nullptr;


    //[Destructor]. You can add your own custom destruction code here..
    buttonGsReset = nullptr;
    buttonGmReset = nullptr;
    //[/Destructor]
}

//==============================================================================
void SettingsComponent::paint (juce::Graphics& g)
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

void SettingsComponent::resized()
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
    //[/UserResized]
}

void SettingsComponent::buttonClicked (juce::Button* buttonThatWasClicked)
{
    //[UserbuttonClicked_Pre]
    if (buttonThatWasClicked == buttonGsReset.get())
    {
        if (onGsReset)
            onGsReset();

        return;
    }

    if (buttonThatWasClicked == buttonGmReset.get())
    {
        if (onGmReset)
            onGmReset();

        return;
    }
    //[/UserbuttonClicked_Pre]

    if (buttonThatWasClicked == buttonClose.get())
    {
        //[UserButtonCode_buttonClose] -- add your button handler code here..
        if (onClose)
            onClose();
        //[/UserButtonCode_buttonClose]
    }
    else if (buttonThatWasClicked == buttonAudioDevice.get())
    {
        //[UserButtonCode_buttonAudioDevice] -- add your button handler code here..
        if (onAudioDeviceSettings)
            onAudioDeviceSettings();
        //[/UserButtonCode_buttonAudioDevice]
    }
    else if (buttonThatWasClicked == buttonLoadRom.get())
    {
        //[UserButtonCode_buttonLoadRom] -- add your button handler code here..
        if (onLoadRom)
            onLoadRom();
        //[/UserButtonCode_buttonLoadRom]
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

<JUCER_COMPONENT documentType="Component" className="SettingsComponent" componentName=""
                 parentClasses="public juce::Component" constructorParams="" variableInitialisers=""
                 scaleOnResize="1" scaleMode="keepAspect" snapPixels="8" snapActive="1"
                 snapShown="1" overlayOpacity="0.330" fixedSize="1" initialWidth="1024"
                 initialHeight="200">
  <BACKGROUND backgroundColour="ff323e44"/>
  <IMAGEBUTTON name="" id="44ae4a19ee6707ae" memberName="buttonClose" virtualName=""
               explicitFocusOrder="0" pos="24 24 72 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::PowerButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::PowerButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::PowerButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <TEXTBUTTON name="new button" id="1612e0a2446926a8" memberName="buttonAudioDevice"
              virtualName="" explicitFocusOrder="0" pos="24 80 272 24" buttonText="Audio Device Settings"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <LABEL name="" id="b3f525f54d13d91a" memberName="labelCurrentRom" virtualName=""
         explicitFocusOrder="0" pos="344 48 208 24" bkgCol="ff000000"
         edTextCol="ff000000" edBkgCol="0" labelText="n/a" editableSingleClick="0"
         editableDoubleClick="0" focusDiscardsChanges="0" fontname="Default font"
         fontsize="15.0" kerning="0.0" bold="0" italic="0" justification="33"/>
  <LABEL name="" id="22d6c36a912254f9" memberName="labelCurrentRomCaption"
         virtualName="" explicitFocusOrder="0" pos="344 16 150 24" edTextCol="ff000000"
         edBkgCol="0" labelText="ROM" editableSingleClick="0" editableDoubleClick="0"
         focusDiscardsChanges="0" fontname="Default font" fontsize="15.0"
         kerning="0.0" bold="0" italic="0" justification="33"/>
  <TEXTBUTTON name="" id="c4b632db3f5c1446" memberName="buttonLoadRom" virtualName=""
              explicitFocusOrder="0" pos="344 80 208 24" buttonText="Load ROM"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
</JUCER_COMPONENT>

END_JUCER_METADATA
*/
#endif


//[EndFile] You can add extra defines here...
//[/EndFile]

