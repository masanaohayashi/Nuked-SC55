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

#include "SC55Component.h"


//[MiscUserDefs] You can add your own user definitions and misc code here...
//[/MiscUserDefs]

//==============================================================================
SC55Component::SC55Component ()
{
    //[Constructor_pre] You can add your own custom stuff here..
    //[/Constructor_pre]

    lcd.reset (new juce::Component());
    addAndMakeVisible (lcd.get());

    lcd->setBounds (260, 38, 344, 124);

    sliderMasterVolume.reset (new juce::Slider (juce::String()));
    addAndMakeVisible (sliderMasterVolume.get());
    sliderMasterVolume->setRange (0, 100, 1);
    sliderMasterVolume->setSliderStyle (juce::Slider::RotaryVerticalDrag);
    sliderMasterVolume->setTextBoxStyle (juce::Slider::NoTextBox, false, 80, 20);
    sliderMasterVolume->addListener (this);
    filmstripSliderLookAndFeel1.setFilmstrip (juce::ImageCache::getFromMemory (BinaryData::Volume_png, BinaryData::Volume_pngSize), 101, true);
    sliderMasterVolume->setLookAndFeel (&filmstripSliderLookAndFeel1);

    sliderMasterVolume->setBounds (132, 24, 64, 64);

    label2x.reset (new juce::Label (juce::String(),
                                    TRANS ("2X")));
    addAndMakeVisible (label2x.get());
    label2x->setFont (juce::Font (juce::FontOptions { 15.00f, juce::Font::plain }.withStyle ("Regular").withMetricsKind (juce::TypefaceMetricsKind::legacy)));
    label2x->setJustificationType (juce::Justification::centredRight);
    label2x->setEditable (false, false, false);
    label2x->setColour (juce::Label::textColourId, juce::Colour (0x80ffffff));
    label2x->setColour (juce::TextEditor::textColourId, juce::Colours::black);
    label2x->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0x00000000));

    label2x->setBounds (648, 112, 46, 16);

    buttonPlayPause.reset (new juce::TextButton (juce::String()));
    addAndMakeVisible (buttonPlayPause.get());
    buttonPlayPause->setButtonText (TRANS ("PLAY"));
    buttonPlayPause->addListener (this);

    buttonPlayPause->setBounds (88, 144, 64, 24);

    buttonStop.reset (new juce::TextButton (juce::String()));
    addAndMakeVisible (buttonStop.get());
    buttonStop->setButtonText (TRANS ("STOP"));
    buttonStop->addListener (this);

    buttonStop->setBounds (16, 144, 64, 24);

    buttonPartDec.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonPartDec.get());
    buttonPartDec->setButtonText (TRANS ("new button"));
    buttonPartDec->addListener (this);

    buttonPartDec->setImages (false, true, true,
                              juce::ImageCache::getFromMemory (BinaryData::PartDecButton_normal_png, BinaryData::PartDecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::PartDecButton_over_png, BinaryData::PartDecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::PartDecButton_down_png, BinaryData::PartDecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonPartDec->setBounds (768, 23, 52, 20);

    buttonPartInc.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonPartInc.get());
    buttonPartInc->addListener (this);

    buttonPartInc->setImages (false, true, true,
                              juce::ImageCache::getFromMemory (BinaryData::PartIncButton_normal_png, BinaryData::PartIncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::PartIncButton_over_png, BinaryData::PartIncButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::PartIncButton_down_png, BinaryData::PartIncButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonPartInc->setBounds (820, 23, 52, 20);

    buttonInstDec.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonInstDec.get());
    buttonInstDec->setButtonText (TRANS ("new button"));
    buttonInstDec->addListener (this);

    buttonInstDec->setImages (false, true, true,
                              juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonInstDec->setBounds (894, 24, 52, 20);

    buttonInstInc.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonInstInc.get());
    buttonInstInc->addListener (this);

    buttonInstInc->setImages (false, true, true,
                              juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                              juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonInstInc->setBounds (946, 24, 52, 20);

    ledPower.reset (new r2juce::R2Led (juce::ImageCache::getFromMemory (BinaryData::Led_png, BinaryData::Led_pngSize), 11));
    addAndMakeVisible (ledPower.get());

    ledPower->setBounds (106, 30, 8, 8);

    buttonMakerLogo.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonMakerLogo.get());
    buttonMakerLogo->addListener (this);

    buttonMakerLogo->setImages (false, true, true,
                                juce::ImageCache::getFromMemory (BinaryData::MakerLogo_png, BinaryData::MakerLogo_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::Image(), 1.000f, juce::Colour (0x00000000),
                                juce::Image(), 1.000f, juce::Colour (0x00000000));
    buttonMakerLogo->setBounds (504, 8, 103, 24);

    buttonSC.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonSC.get());
    buttonSC->addListener (this);

    buttonSC->setImages (false, true, true,
                         juce::Image(), 1.000f, juce::Colour (0x00000000),
                         juce::Image(), 1.000f, juce::Colour (0x00000000),
                         juce::Image(), 1.000f, juce::Colour (0x00000000));
    buttonSC->setBounds (251, 166, 192, 24);

    buttonMk2.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonMk2.get());
    buttonMk2->addListener (this);

    buttonMk2->setImages (false, true, true,
                          juce::ImageCache::getFromMemory (BinaryData::Logo_SC155_png, BinaryData::Logo_SC155_pngSize), 1.000f, juce::Colour (0x00000000),
                          juce::Image(), 1.000f, juce::Colour (0x00000000),
                          juce::Image(), 1.000f, juce::Colour (0x00000000));
    buttonMk2->setBounds (484, 169, 125, 20);

    buttonAll.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonAll.get());
    buttonAll->setButtonText (TRANS ("new button"));
    buttonAll->addListener (this);

    buttonAll->setImages (false, true, true,
                          juce::ImageCache::getFromMemory (BinaryData::LedButton_off_png, BinaryData::LedButton_off_pngSize), 1.000f, juce::Colour (0x00000000),
                          juce::ImageCache::getFromMemory (BinaryData::LedButton_off_png, BinaryData::LedButton_off_pngSize), 1.000f, juce::Colour (0x00000000),
                          juce::ImageCache::getFromMemory (BinaryData::LedButton_on_png, BinaryData::LedButton_on_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonAll->setBounds (696, 22, 24, 24);

    buttonMute.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonMute.get());
    buttonMute->setButtonText (TRANS ("new button"));
    buttonMute->addListener (this);

    buttonMute->setImages (false, true, true,
                           juce::ImageCache::getFromMemory (BinaryData::LedButton_off_png, BinaryData::LedButton_off_pngSize), 1.000f, juce::Colour (0x00000000),
                           juce::ImageCache::getFromMemory (BinaryData::LedButton_off_png, BinaryData::LedButton_off_pngSize), 1.000f, juce::Colour (0x00000000),
                           juce::ImageCache::getFromMemory (BinaryData::LedButton_on_png, BinaryData::LedButton_on_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonMute->setBounds (696, 64, 24, 24);

    button2x.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (button2x.get());
    button2x->addListener (this);

    button2x->setImages (false, true, true,
                         juce::ImageCache::getFromMemory (BinaryData::LedButton_off_png, BinaryData::LedButton_off_pngSize), 1.000f, juce::Colour (0x00000000),
                         juce::ImageCache::getFromMemory (BinaryData::LedButton_off_png, BinaryData::LedButton_off_pngSize), 1.000f, juce::Colour (0x00000000),
                         juce::ImageCache::getFromMemory (BinaryData::LedButton_on_png, BinaryData::LedButton_on_pngSize), 1.000f, juce::Colour (0x00000000));
    button2x->setBounds (696, 108, 24, 24);

    buttonPower.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonPower.get());
    buttonPower->addListener (this);

    buttonPower->setImages (false, true, true,
                            juce::ImageCache::getFromMemory (BinaryData::PowerButton_normal_png, BinaryData::PowerButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                            juce::ImageCache::getFromMemory (BinaryData::PowerButton_over_png, BinaryData::PowerButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                            juce::ImageCache::getFromMemory (BinaryData::PowerButton_down_png, BinaryData::PowerButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonPower->setBounds (24, 24, 72, 20);

    buttonLevelDec.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonLevelDec.get());
    buttonLevelDec->setButtonText (TRANS ("new button"));
    buttonLevelDec->addListener (this);

    buttonLevelDec->setImages (false, true, true,
                               juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonLevelDec->setBounds (768, 67, 52, 20);

    buttonLevelInc.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonLevelInc.get());
    buttonLevelInc->addListener (this);

    buttonLevelInc->setImages (false, true, true,
                               juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                               juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonLevelInc->setBounds (820, 67, 52, 20);

    buttonPanDec.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonPanDec.get());
    buttonPanDec->setButtonText (TRANS ("new button"));
    buttonPanDec->addListener (this);

    buttonPanDec->setImages (false, true, true,
                             juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                             juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                             juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonPanDec->setBounds (894, 67, 52, 20);

    buttonPanInc.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonPanInc.get());
    buttonPanInc->addListener (this);

    buttonPanInc->setImages (false, true, true,
                             juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                             juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                             juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonPanInc->setBounds (946, 67, 52, 20);

    buttonReverbDec.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonReverbDec.get());
    buttonReverbDec->setButtonText (TRANS ("new button"));
    buttonReverbDec->addListener (this);

    buttonReverbDec->setImages (false, true, true,
                                juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonReverbDec->setBounds (768, 110, 52, 20);

    buttonReverbInc.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonReverbInc.get());
    buttonReverbInc->addListener (this);

    buttonReverbInc->setImages (false, true, true,
                                juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonReverbInc->setBounds (820, 110, 52, 20);

    buttonChorusDec.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonChorusDec.get());
    buttonChorusDec->setButtonText (TRANS ("new button"));
    buttonChorusDec->addListener (this);

    buttonChorusDec->setImages (false, true, true,
                                juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonChorusDec->setBounds (894, 110, 52, 20);

    buttonChorusInc.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonChorusInc.get());
    buttonChorusInc->addListener (this);

    buttonChorusInc->setImages (false, true, true,
                                juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonChorusInc->setBounds (946, 110, 52, 20);

    buttonKeyShiftDec.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonKeyShiftDec.get());
    buttonKeyShiftDec->setButtonText (TRANS ("new button"));
    buttonKeyShiftDec->addListener (this);

    buttonKeyShiftDec->setImages (false, true, true,
                                  juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                  juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                                  juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonKeyShiftDec->setBounds (768, 154, 52, 20);

    buttonKeyShiftInc.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonKeyShiftInc.get());
    buttonKeyShiftInc->addListener (this);

    buttonKeyShiftInc->setImages (false, true, true,
                                  juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                  juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                                  juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonKeyShiftInc->setBounds (820, 154, 52, 20);

    buttonMidiChDec.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonMidiChDec.get());
    buttonMidiChDec->setButtonText (TRANS ("new button"));
    buttonMidiChDec->addListener (this);

    buttonMidiChDec->setImages (false, true, true,
                                juce::ImageCache::getFromMemory (BinaryData::DecButton_normal_png, BinaryData::DecButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::DecButton_over_png, BinaryData::DecButton_over_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::DecButton_down_png, BinaryData::DecButton_down_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonMidiChDec->setBounds (894, 154, 52, 20);

    buttonMidiChInc.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonMidiChInc.get());
    buttonMidiChInc->addListener (this);

    buttonMidiChInc->setImages (false, true, true,
                                juce::ImageCache::getFromMemory (BinaryData::IncButton_normal_png, BinaryData::IncButton_normal_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::IncButton_down_png, BinaryData::IncButton_down_pngSize), 1.000f, juce::Colour (0x00000000),
                                juce::ImageCache::getFromMemory (BinaryData::IncButton_over_png, BinaryData::IncButton_over_pngSize), 1.000f, juce::Colour (0x00000000));
    buttonMidiChInc->setBounds (946, 154, 52, 20);

    buttonLoad.reset (new juce::TextButton (juce::String()));
    addAndMakeVisible (buttonLoad.get());
    buttonLoad->setButtonText (TRANS ("LOAD"));
    buttonLoad->addListener (this);

    buttonLoad->setBounds (160, 144, 64, 24);

    labelPlayer.reset (new juce::Label (juce::String(),
                                        TRANS ("RCP/MID PLAYER")));
    addAndMakeVisible (labelPlayer.get());
    labelPlayer->setFont (juce::Font (juce::FontOptions { 15.00f, juce::Font::plain }.withStyle ("Regular").withMetricsKind (juce::TypefaceMetricsKind::legacy)));
    labelPlayer->setJustificationType (juce::Justification::centredLeft);
    labelPlayer->setEditable (false, false, false);
    labelPlayer->setColour (juce::Label::textColourId, juce::Colour (0x80ffffff));
    labelPlayer->setColour (juce::TextEditor::textColourId, juce::Colours::black);
    labelPlayer->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0x00000000));

    labelPlayer->setBounds (16, 120, 144, 16);

    buttonGM.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonGM.get());
    buttonGM->addListener (this);

    buttonGM->setImages (false, true, true,
                         juce::ImageCache::getFromMemory (BinaryData::GMButton_png, BinaryData::GMButton_pngSize), 1.000f, juce::Colour (0x00000000),
                         juce::Image(), 1.000f, juce::Colour (0x00000000),
                         juce::Image(), 1.000f, juce::Colour (0x00000000));
    buttonGM->setBounds (649, 164, 40, 28);

    buttonGS.reset (new juce::ImageButton (juce::String()));
    addAndMakeVisible (buttonGS.get());
    buttonGS->addListener (this);

    buttonGS->setImages (false, true, true,
                         juce::ImageCache::getFromMemory (BinaryData::GSButton_png, BinaryData::GSButton_pngSize), 1.000f, juce::Colour (0x00000000),
                         juce::Image(), 1.000f, juce::Colour (0x00000000),
                         juce::Image(), 1.000f, juce::Colour (0x00000000));
    buttonGS->setBounds (695, 164, 40, 28);

    cachedImage_BinaryData_Background_png_2 = juce::ImageCache::getFromMemory (BinaryData::Background_png, BinaryData::Background_pngSize);

    //[UserPreSize]
    //[/UserPreSize]

    setSize (1024, 200);


    //[Constructor] You can add your own custom stuff here..
    //[/Constructor]
}

SC55Component::~SC55Component()
{
    //[Destructor_pre]. You can add your own custom destruction code here..
    //[/Destructor_pre]

    lcd = nullptr;
    sliderMasterVolume->setLookAndFeel (nullptr);
    sliderMasterVolume = nullptr;
    label2x = nullptr;
    buttonPlayPause = nullptr;
    buttonStop = nullptr;
    buttonPartDec = nullptr;
    buttonPartInc = nullptr;
    buttonInstDec = nullptr;
    buttonInstInc = nullptr;
    ledPower = nullptr;
    buttonMakerLogo = nullptr;
    buttonSC = nullptr;
    buttonMk2 = nullptr;
    buttonAll = nullptr;
    buttonMute = nullptr;
    button2x = nullptr;
    buttonPower = nullptr;
    buttonLevelDec = nullptr;
    buttonLevelInc = nullptr;
    buttonPanDec = nullptr;
    buttonPanInc = nullptr;
    buttonReverbDec = nullptr;
    buttonReverbInc = nullptr;
    buttonChorusDec = nullptr;
    buttonChorusInc = nullptr;
    buttonKeyShiftDec = nullptr;
    buttonKeyShiftInc = nullptr;
    buttonMidiChDec = nullptr;
    buttonMidiChInc = nullptr;
    buttonLoad = nullptr;
    labelPlayer = nullptr;
    buttonGM = nullptr;
    buttonGS = nullptr;


    //[Destructor]. You can add your own custom destruction code here..
    //[/Destructor]
}

//==============================================================================
void SC55Component::paint (juce::Graphics& g)
{
    //[UserPrePaint] Add your own custom painting code here..
    //[/UserPrePaint]

    g.fillAll (juce::Colours::black);

    {
        int x = 0, y = 0, width = 1024, height = 200;
        //[UserPaintCustomArguments] Customize the painting arguments here..
        //[/UserPaintCustomArguments]
        g.setColour (juce::Colours::black);
        g.drawImage (cachedImage_BinaryData_Background_png_2,
                     x, y, width, height,
                     0, 0, cachedImage_BinaryData_Background_png_2.getWidth(), cachedImage_BinaryData_Background_png_2.getHeight());
    }

    //[UserPaint] Add your own custom painting code here..
    //[/UserPaint]
}

void SC55Component::resized()
{
    //[UserPreResize] Add your own custom resize code here..
    //[/UserPreResize]

    //[UserResized] Add your own custom resize handling here..
    //[/UserResized]
}

void SC55Component::sliderValueChanged (juce::Slider* sliderThatWasMoved)
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

void SC55Component::buttonClicked (juce::Button* buttonThatWasClicked)
{
    //[UserbuttonClicked_Pre]
    //[/UserbuttonClicked_Pre]

    if (buttonThatWasClicked == buttonPlayPause.get())
    {
        //[UserButtonCode_buttonPlayPause] -- add your button handler code here..
        //[/UserButtonCode_buttonPlayPause]
    }
    else if (buttonThatWasClicked == buttonStop.get())
    {
        //[UserButtonCode_buttonStop] -- add your button handler code here..
        //[/UserButtonCode_buttonStop]
    }
    else if (buttonThatWasClicked == buttonPartDec.get())
    {
        //[UserButtonCode_buttonPartDec] -- add your button handler code here..
        //[/UserButtonCode_buttonPartDec]
    }
    else if (buttonThatWasClicked == buttonPartInc.get())
    {
        //[UserButtonCode_buttonPartInc] -- add your button handler code here..
        //[/UserButtonCode_buttonPartInc]
    }
    else if (buttonThatWasClicked == buttonInstDec.get())
    {
        //[UserButtonCode_buttonInstDec] -- add your button handler code here..
        //[/UserButtonCode_buttonInstDec]
    }
    else if (buttonThatWasClicked == buttonInstInc.get())
    {
        //[UserButtonCode_buttonInstInc] -- add your button handler code here..
        //[/UserButtonCode_buttonInstInc]
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
    else if (buttonThatWasClicked == buttonAll.get())
    {
        //[UserButtonCode_buttonAll] -- add your button handler code here..
        //[/UserButtonCode_buttonAll]
    }
    else if (buttonThatWasClicked == buttonMute.get())
    {
        //[UserButtonCode_buttonMute] -- add your button handler code here..
        //[/UserButtonCode_buttonMute]
    }
    else if (buttonThatWasClicked == button2x.get())
    {
        //[UserButtonCode_button2x] -- add your button handler code here..
        //[/UserButtonCode_button2x]
    }
    else if (buttonThatWasClicked == buttonPower.get())
    {
        //[UserButtonCode_buttonPower] -- add your button handler code here..
        //[/UserButtonCode_buttonPower]
    }
    else if (buttonThatWasClicked == buttonLevelDec.get())
    {
        //[UserButtonCode_buttonLevelDec] -- add your button handler code here..
        //[/UserButtonCode_buttonLevelDec]
    }
    else if (buttonThatWasClicked == buttonLevelInc.get())
    {
        //[UserButtonCode_buttonLevelInc] -- add your button handler code here..
        //[/UserButtonCode_buttonLevelInc]
    }
    else if (buttonThatWasClicked == buttonPanDec.get())
    {
        //[UserButtonCode_buttonPanDec] -- add your button handler code here..
        //[/UserButtonCode_buttonPanDec]
    }
    else if (buttonThatWasClicked == buttonPanInc.get())
    {
        //[UserButtonCode_buttonPanInc] -- add your button handler code here..
        //[/UserButtonCode_buttonPanInc]
    }
    else if (buttonThatWasClicked == buttonReverbDec.get())
    {
        //[UserButtonCode_buttonReverbDec] -- add your button handler code here..
        //[/UserButtonCode_buttonReverbDec]
    }
    else if (buttonThatWasClicked == buttonReverbInc.get())
    {
        //[UserButtonCode_buttonReverbInc] -- add your button handler code here..
        //[/UserButtonCode_buttonReverbInc]
    }
    else if (buttonThatWasClicked == buttonChorusDec.get())
    {
        //[UserButtonCode_buttonChorusDec] -- add your button handler code here..
        //[/UserButtonCode_buttonChorusDec]
    }
    else if (buttonThatWasClicked == buttonChorusInc.get())
    {
        //[UserButtonCode_buttonChorusInc] -- add your button handler code here..
        //[/UserButtonCode_buttonChorusInc]
    }
    else if (buttonThatWasClicked == buttonKeyShiftDec.get())
    {
        //[UserButtonCode_buttonKeyShiftDec] -- add your button handler code here..
        //[/UserButtonCode_buttonKeyShiftDec]
    }
    else if (buttonThatWasClicked == buttonKeyShiftInc.get())
    {
        //[UserButtonCode_buttonKeyShiftInc] -- add your button handler code here..
        //[/UserButtonCode_buttonKeyShiftInc]
    }
    else if (buttonThatWasClicked == buttonMidiChDec.get())
    {
        //[UserButtonCode_buttonMidiChDec] -- add your button handler code here..
        //[/UserButtonCode_buttonMidiChDec]
    }
    else if (buttonThatWasClicked == buttonMidiChInc.get())
    {
        //[UserButtonCode_buttonMidiChInc] -- add your button handler code here..
        //[/UserButtonCode_buttonMidiChInc]
    }
    else if (buttonThatWasClicked == buttonLoad.get())
    {
        //[UserButtonCode_buttonLoad] -- add your button handler code here..
        //[/UserButtonCode_buttonLoad]
    }
    else if (buttonThatWasClicked == buttonGM.get())
    {
        //[UserButtonCode_buttonGM] -- add your button handler code here..
        //[/UserButtonCode_buttonGM]
    }
    else if (buttonThatWasClicked == buttonGS.get())
    {
        //[UserButtonCode_buttonGS] -- add your button handler code here..
        //[/UserButtonCode_buttonGS]
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

<JUCER_COMPONENT documentType="Component" className="SC55Component" componentName=""
                 parentClasses="public juce::Component" constructorParams="" variableInitialisers=""
                 snapPixels="8" snapActive="1" snapShown="1" overlayOpacity="0.330"
                 fixedSize="1" initialWidth="1024" initialHeight="200">
  <BACKGROUND backgroundColour="ff000000">
    <IMAGE pos="0 0 1024 200" resource="BinaryData::Background_png" opacity="1.0"
           mode="0"/>
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
  <IMAGEBUTTON name="" id="bd7a7d1e8ecefc56" memberName="buttonPartDec" virtualName=""
               explicitFocusOrder="0" pos="768 23 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::PartDecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::PartDecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::PartDecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="e5595339a02d2464" memberName="buttonPartInc" virtualName=""
               explicitFocusOrder="0" pos="820 23 52 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::PartIncButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::PartIncButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::PartIncButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="3c18f144f8b62f68" memberName="buttonInstDec" virtualName=""
               explicitFocusOrder="0" pos="894 24 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="5bdf0a672aeba4a0" memberName="buttonInstInc" virtualName=""
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
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal=""
               opacityNormal="1.0" colourNormal="0" resourceOver="" opacityOver="1.0"
               colourOver="0" resourceDown="" opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="b7f4c2171b20f4e1" memberName="buttonMk2" virtualName=""
               explicitFocusOrder="0" pos="484 169 125 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::Logo_SC155_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="" opacityOver="1.0"
               colourOver="0" resourceDown="" opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="7e618566428e4ed6" memberName="buttonAll" virtualName=""
               explicitFocusOrder="0" pos="696 22 24 24" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::LedButton_off_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::LedButton_off_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::LedButton_on_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="40fe940c238ae3ca" memberName="buttonMute" virtualName=""
               explicitFocusOrder="0" pos="696 64 24 24" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::LedButton_off_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::LedButton_off_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::LedButton_on_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="a87ba9651dd9c626" memberName="button2x" virtualName=""
               explicitFocusOrder="0" pos="696 108 24 24" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::LedButton_off_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::LedButton_off_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::LedButton_on_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="44ae4a19ee6707ae" memberName="buttonPower" virtualName=""
               explicitFocusOrder="0" pos="24 24 72 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::PowerButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::PowerButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::PowerButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="c3b257319e397d96" memberName="buttonLevelDec" virtualName=""
               explicitFocusOrder="0" pos="768 67 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="4806a33c53deff13" memberName="buttonLevelInc" virtualName=""
               explicitFocusOrder="0" pos="820 67 52 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::IncButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::IncButton_down_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::IncButton_over_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="d89f1d7b86b80be3" memberName="buttonPanDec" virtualName=""
               explicitFocusOrder="0" pos="894 67 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="a0cb50c25e14cf63" memberName="buttonPanInc" virtualName=""
               explicitFocusOrder="0" pos="946 67 52 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::IncButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::IncButton_down_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::IncButton_over_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="743751e5ee71c550" memberName="buttonReverbDec" virtualName=""
               explicitFocusOrder="0" pos="768 110 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="f896f520f9905fa8" memberName="buttonReverbInc" virtualName=""
               explicitFocusOrder="0" pos="820 110 52 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::IncButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::IncButton_down_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::IncButton_over_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="bfa466bbf4b0a117" memberName="buttonChorusDec" virtualName=""
               explicitFocusOrder="0" pos="894 110 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="58ddc8d78d0c69e4" memberName="buttonChorusInc" virtualName=""
               explicitFocusOrder="0" pos="946 110 52 20" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::IncButton_normal_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="BinaryData::IncButton_down_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::IncButton_over_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="4a67d7a98526be72" memberName="buttonKeyShiftDec"
               virtualName="" explicitFocusOrder="0" pos="768 154 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="a7905cd531c3d999" memberName="buttonKeyShiftInc"
               virtualName="" explicitFocusOrder="0" pos="820 154 52 20" buttonText=""
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::IncButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::IncButton_down_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::IncButton_over_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="4b095cd63fef7324" memberName="buttonMidiChDec" virtualName=""
               explicitFocusOrder="0" pos="894 154 52 20" buttonText="new button"
               connectedEdges="0" needsCallback="1" radioGroupId="0" keepProportions="1"
               resourceNormal="BinaryData::DecButton_normal_png" opacityNormal="1.0"
               colourNormal="0" resourceOver="BinaryData::DecButton_over_png"
               opacityOver="1.0" colourOver="0" resourceDown="BinaryData::DecButton_down_png"
               opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="5892b2fed7947741" memberName="buttonMidiChInc" virtualName=""
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
  <IMAGEBUTTON name="" id="a5a6da76c1066560" memberName="buttonGM" virtualName=""
               explicitFocusOrder="0" pos="649 164 40 28" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::GMButton_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="" opacityOver="1.0"
               colourOver="0" resourceDown="" opacityDown="1.0" colourDown="0"/>
  <IMAGEBUTTON name="" id="dbdf6ea51dc8983d" memberName="buttonGS" virtualName=""
               explicitFocusOrder="0" pos="695 164 40 28" buttonText="" connectedEdges="0"
               needsCallback="1" radioGroupId="0" keepProportions="1" resourceNormal="BinaryData::GSButton_png"
               opacityNormal="1.0" colourNormal="0" resourceOver="" opacityOver="1.0"
               colourOver="0" resourceDown="" opacityDown="1.0" colourDown="0"/>
</JUCER_COMPONENT>

END_JUCER_METADATA
*/
#endif


//[EndFile] You can add extra defines here...
//[/EndFile]

