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

    labelCurrentRomCaption.reset (new juce::Label (juce::String(),
                                                   TRANS ("ROM")));
    contentComponent.addAndMakeVisible (labelCurrentRomCaption.get());
    labelCurrentRomCaption->setFont (juce::Font (juce::FontOptions { 15.00f, juce::Font::plain }.withStyle ("Regular").withMetricsKind (juce::TypefaceMetricsKind::legacy)));
    labelCurrentRomCaption->setJustificationType (juce::Justification::centredLeft);
    labelCurrentRomCaption->setEditable (false, false, false);
    labelCurrentRomCaption->setColour (juce::TextEditor::textColourId, juce::Colours::black);
    labelCurrentRomCaption->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0x00000000));

    labelCurrentRomCaption->setBounds (120, 24, 208, 24);

    comboRoms.reset (new ImportAwareComboBox (juce::String()));
    contentComponent.addAndMakeVisible (comboRoms.get());
    comboRoms->setEditableText (false);
    comboRoms->setJustificationType (juce::Justification::centredLeft);
    comboRoms->setTextWhenNothingSelected (juce::String());
    comboRoms->setTextWhenNoChoicesAvailable (TRANS ("(no choices)"));
    comboRoms->addListener (this);

    comboRoms->setBounds (120, 48, 208, 24);

    juce__label.reset (new juce::Label ("new label",
                                        TRANS ("CLOSE")));
    contentComponent.addAndMakeVisible (juce__label.get());
    juce__label->setFont (juce::Font (juce::FontOptions { 15.00f, juce::Font::plain }.withStyle ("Regular").withMetricsKind (juce::TypefaceMetricsKind::legacy)));
    juce__label->setJustificationType (juce::Justification::centred);
    juce__label->setEditable (false, false, false);
    juce__label->setColour (juce::TextEditor::textColourId, juce::Colours::black);
    juce__label->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0x00000000));

    juce__label->setBounds (24, 0, 72, 24);

    viewport.reset (new juce::Viewport ("viewport"));
    contentComponent.addAndMakeVisible (viewport.get());

    viewport->setBounds (384, 0, 640, 200);


    //[UserPreSize]
    //[/UserPreSize]

    setSize (1024, 200);


    //[Constructor] You can add your own custom stuff here..
    viewport->setVisible (false);

    labelCurrentRomCaption->setColour (juce::Label::textColourId,
                                       juce::Colours::white.withAlpha (0.7f));
    //[/Constructor]
}

SettingsComponent::~SettingsComponent()
{
    //[Destructor_pre]. You can add your own custom destruction code here..
    //[/Destructor_pre]

    buttonClose = nullptr;
    labelCurrentRomCaption = nullptr;
    comboRoms = nullptr;
    juce__label = nullptr;
    viewport = nullptr;


    //[Destructor]. You can add your own custom destruction code here..
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
    if (audioDeviceSettings != nullptr && viewport != nullptr)
        audioDeviceSettings->setSize (viewport->getWidth(), 512);
    //[/UserResized]
}

void SettingsComponent::buttonClicked (juce::Button* buttonThatWasClicked)
{
    //[UserbuttonClicked_Pre]
    //[/UserbuttonClicked_Pre]

    if (buttonThatWasClicked == buttonClose.get())
    {
        //[UserButtonCode_buttonClose] -- add your button handler code here..
        if (onClose)
            onClose();
        //[/UserButtonCode_buttonClose]
    }

    //[UserbuttonClicked_Post]
    //[/UserbuttonClicked_Post]
}

void SettingsComponent::comboBoxChanged (juce::ComboBox* comboBoxThatHasChanged)
{
    //[UsercomboBoxChanged_Pre]
    //[/UsercomboBoxChanged_Pre]

    if (comboBoxThatHasChanged == comboRoms.get())
    {
        //[UserComboBoxCode_comboRoms] -- add your combo box handling code here..
        const auto selectedId = comboRoms->getSelectedId();
        if (selectedId == importRomItemId)
        {
            const auto previousSelection = selectedRomName;
            setSelectedRomName (previousSelection);

            if (onImportRom)
                onImportRom();
        }
        else if (selectedId > 0)
        {
            selectedRomName = comboRoms->getText();
            if (onRomSelected)
                onRomSelected (selectedRomName);
        }
        //[/UserComboBoxCode_comboRoms]
    }

    //[UsercomboBoxChanged_Post]
    //[/UsercomboBoxChanged_Post]
}



//[MiscUserCode] You can add your own definitions of your custom methods or any other code here...
void SettingsComponent::setAudioDeviceManager (juce::AudioDeviceManager* manager)
{
    if (viewport == nullptr)
        return;

    viewport->setViewedComponent (nullptr, false);
    audioDeviceSettings = nullptr;

    if (manager != nullptr)
    {
        audioDeviceSettings = std::make_unique<juce::AudioDeviceSelectorComponent> (
            *manager,
            0, 0,
            0, 2,
            true, false,
            true, false);
        audioDeviceSettings->setSize (viewport->getWidth(), 512);
        viewport->setViewedComponent (audioDeviceSettings.get(), false);
    }

    viewport->setVisible (audioDeviceSettings != nullptr);
}

void SettingsComponent::setRomChoices (const juce::StringArray& names,
                                       const juce::String& selectedName)
{
    selectedRomName = selectedName;

    if (comboRoms == nullptr)
        return;

    comboRoms->setImportOnly (names.isEmpty());
    comboRoms->setEnabled (true);
    comboRoms->setTextWhenNothingSelected (names.isEmpty()
                                                ? TRANS ("Import ROM")
                                                : juce::String());
    comboRoms->clear (juce::dontSendNotification);
    comboRoms->addItemList (names, 1);
    if (! names.isEmpty())
        comboRoms->addSeparator();
    comboRoms->addItem (TRANS ("Import ROM"), importRomItemId);
    setSelectedRomName (selectedName);

    DBG ("[DEBUG-SC55] ROM combo refreshed names=" + juce::String (names.size())
         + " items=" + juce::String (comboRoms->getNumItems())
         + " importOnly=" + juce::String (names.isEmpty() ? 1 : 0));
}

void SettingsComponent::setSelectedRomName (const juce::String& name)
{
    selectedRomName = name;

    if (comboRoms == nullptr)
        return;

    for (int index = 0; index < comboRoms->getNumItems(); ++index)
    {
        if (comboRoms->getItemText (index) == name)
        {
            comboRoms->setSelectedId (comboRoms->getItemId (index),
                                      juce::dontSendNotification);
            return;
        }
    }

    comboRoms->setText (name, juce::dontSendNotification);
}
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
  <LABEL name="" id="22d6c36a912254f9" memberName="labelCurrentRomCaption"
         virtualName="" explicitFocusOrder="0" pos="120 24 208 24" edTextCol="ff000000"
         edBkgCol="0" labelText="ROM" editableSingleClick="0" editableDoubleClick="0"
         focusDiscardsChanges="0" fontname="Default font" fontsize="15.0"
         kerning="0.0" bold="0" italic="0" justification="33"/>
  <COMBOBOX name="" id="fc40b79ca6ae887" memberName="comboRoms" virtualName=""
            explicitFocusOrder="0" pos="120 48 208 24" editable="0" layout="33"
            items="" textWhenNonSelected="" textWhenNoItems="(no choices)"/>
  <LABEL name="new label" id="9df59162551224f7" memberName="juce__label"
         virtualName="" explicitFocusOrder="0" pos="24 0 72 24" edTextCol="ff000000"
         edBkgCol="0" labelText="CLOSE" editableSingleClick="0" editableDoubleClick="0"
         focusDiscardsChanges="0" fontname="Default font" fontsize="15.0"
         kerning="0.0" bold="0" italic="0" justification="36"/>
  <VIEWPORT name="viewport" id="2e02ecad0806a8a3" memberName="viewport" virtualName=""
            explicitFocusOrder="0" pos="384 0 640 200" vscroll="1" hscroll="1"
            scrollbarThickness="8" contentType="0" jucerFile="" contentClass=""
            constructorParams=""/>
</JUCER_COMPONENT>

END_JUCER_METADATA
*/
#endif


//[EndFile] You can add extra defines here...
//[/EndFile]

