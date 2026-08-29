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
        displayEnabled = processor.copyLcdDisplay (displayMask.data(), LCD_DISPLAY_WIDTH);

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

    buttonLevelDec.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonLevelDec.get());
    buttonLevelDec->setButtonText (TRANS ("<"));
    buttonLevelDec->addListener (this);

    buttonLevelDec->setBounds (768, 67, 52, 20);

    buttonLevelInc.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonLevelInc.get());
    buttonLevelInc->setButtonText (TRANS (">"));
    buttonLevelInc->addListener (this);

    buttonLevelInc->setBounds (820, 67, 52, 20);

    buttonReverbDec.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonReverbDec.get());
    buttonReverbDec->setButtonText (TRANS ("<"));
    buttonReverbDec->addListener (this);

    buttonReverbDec->setBounds (768, 110, 52, 20);

    buttonReverbInc.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonReverbInc.get());
    buttonReverbInc->setButtonText (TRANS (">"));
    buttonReverbInc->addListener (this);

    buttonReverbInc->setBounds (820, 110, 52, 20);

    buttonPartDec.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonPartDec.get());
    buttonPartDec->setButtonText (TRANS ("<"));
    buttonPartDec->addListener (this);

    buttonPartDec->setBounds (768, 24, 52, 20);

    buttonPartInc.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonPartInc.get());
    buttonPartInc->setButtonText (TRANS (">"));
    buttonPartInc->addListener (this);

    buttonPartInc->setBounds (820, 24, 52, 20);

    buttonKeyShiftDec.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonKeyShiftDec.get());
    buttonKeyShiftDec->setButtonText (TRANS ("<"));
    buttonKeyShiftDec->addListener (this);

    buttonKeyShiftDec->setBounds (768, 154, 52, 20);

    buttonKeyShiftInc.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonKeyShiftInc.get());
    buttonKeyShiftInc->setButtonText (TRANS (">"));
    buttonKeyShiftInc->addListener (this);

    buttonKeyShiftInc->setBounds (820, 154, 52, 20);

    buttonAll.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonAll.get());
    buttonAll->addListener (this);

    buttonAll->setBounds (696, 22, 24, 24);

    buttonAll2.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonAll2.get());
    buttonAll2->addListener (this);

    buttonAll2->setBounds (696, 65, 24, 24);

    buttonPanDec.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonPanDec.get());
    buttonPanDec->setButtonText (TRANS ("<"));
    buttonPanDec->addListener (this);

    buttonPanDec->setBounds (894, 67, 52, 20);

    buttonPanInc.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonPanInc.get());
    buttonPanInc->setButtonText (TRANS (">"));
    buttonPanInc->addListener (this);

    buttonPanInc->setBounds (946, 67, 52, 20);

    buttonChorusDec.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonChorusDec.get());
    buttonChorusDec->setButtonText (TRANS ("<"));
    buttonChorusDec->addListener (this);

    buttonChorusDec->setBounds (894, 110, 52, 20);

    buttonChorusInc.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonChorusInc.get());
    buttonChorusInc->setButtonText (TRANS (">"));
    buttonChorusInc->addListener (this);

    buttonChorusInc->setBounds (946, 110, 52, 20);

    buttonInstDec.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonInstDec.get());
    buttonInstDec->setButtonText (TRANS ("<"));
    buttonInstDec->addListener (this);

    buttonInstDec->setBounds (894, 24, 52, 20);

    buttonInstInc.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonInstInc.get());
    buttonInstInc->setButtonText (TRANS (">"));
    buttonInstInc->addListener (this);

    buttonInstInc->setBounds (946, 24, 52, 20);

    buttonMidiChDec.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonMidiChDec.get());
    buttonMidiChDec->setButtonText (TRANS ("<"));
    buttonMidiChDec->addListener (this);

    buttonMidiChDec->setBounds (894, 154, 52, 20);

    buttonMidiChInc.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonMidiChInc.get());
    buttonMidiChInc->setButtonText (TRANS (">"));
    buttonMidiChInc->addListener (this);

    buttonMidiChInc->setBounds (946, 154, 52, 20);

    buttonPower.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonPower.get());
    buttonPower->addListener (this);

    buttonPower->setBounds (24, 24, 72, 24);

    sliderMasterVolume.reset (new juce::Slider (juce::String()));
    contentComponent.addAndMakeVisible (sliderMasterVolume.get());
    sliderMasterVolume->setRange (0, 100, 1);
    sliderMasterVolume->setSliderStyle (juce::Slider::RotaryVerticalDrag);
    sliderMasterVolume->setTextBoxStyle (juce::Slider::NoTextBox, false, 80, 20);
    sliderMasterVolume->addListener (this);
    filmstripSliderLookAndFeel1.setFilmstrip (juce::ImageCache::getFromMemory (BinaryData::Volume_png, BinaryData::Volume_pngSize), 101, true);
    sliderMasterVolume->setLookAndFeel (&filmstripSliderLookAndFeel1);

    sliderMasterVolume->setBounds (132, 24, 64, 64);

    button2x.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (button2x.get());
    button2x->addListener (this);

    button2x->setBounds (696, 108, 24, 24);

    label2x.reset (new juce::Label (juce::String(),
                                    TRANS ("2X")));
    contentComponent.addAndMakeVisible (label2x.get());
    label2x->setFont (juce::Font (juce::FontOptions { 15.00f, juce::Font::plain }.withStyle ("Regular").withMetricsKind (juce::TypefaceMetricsKind::legacy)));
    label2x->setJustificationType (juce::Justification::centredRight);
    label2x->setEditable (false, false, false);
    label2x->setColour (juce::TextEditor::textColourId, juce::Colours::black);
    label2x->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0x00000000));

    label2x->setBounds (648, 112, 46, 16);

    buttonPlayPause.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonPlayPause.get());
    buttonPlayPause->setButtonText (TRANS ("PLAY"));
    buttonPlayPause->addListener (this);

    buttonPlayPause->setBounds (120, 120, 80, 24);

    buttonStop.reset (new juce::TextButton (juce::String()));
    contentComponent.addAndMakeVisible (buttonStop.get());
    buttonStop->setButtonText (TRANS ("STOP"));
    buttonStop->addListener (this);

    buttonStop->setBounds (24, 120, 80, 24);

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
    button2x->setClickingTogglesState (true);
    button2x->setToggleState (audioProcessor.isTwoXEnabled(), juce::dontSendNotification);
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
    buttonLevelDec = nullptr;
    buttonLevelInc = nullptr;
    buttonReverbDec = nullptr;
    buttonReverbInc = nullptr;
    buttonPartDec = nullptr;
    buttonPartInc = nullptr;
    buttonKeyShiftDec = nullptr;
    buttonKeyShiftInc = nullptr;
    buttonAll = nullptr;
    buttonAll2 = nullptr;
    buttonPanDec = nullptr;
    buttonPanInc = nullptr;
    buttonChorusDec = nullptr;
    buttonChorusInc = nullptr;
    buttonInstDec = nullptr;
    buttonInstInc = nullptr;
    buttonMidiChDec = nullptr;
    buttonMidiChInc = nullptr;
    buttonPower = nullptr;
    sliderMasterVolume->setLookAndFeel (nullptr);
    sliderMasterVolume = nullptr;
    button2x = nullptr;
    label2x = nullptr;
    buttonPlayPause = nullptr;
    buttonStop = nullptr;


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
        g.drawImage (cachedImage_BinaryData_Background_png_2,
                     x, y, width, height,
                     0, 0, cachedImage_BinaryData_Background_png_2.getWidth(), cachedImage_BinaryData_Background_png_2.getHeight());
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

void NukedSC55AudioProcessorEditor::buttonClicked (juce::Button* buttonThatWasClicked)
{
    //[UserbuttonClicked_Pre]
    //[/UserbuttonClicked_Pre]

    if (buttonThatWasClicked == buttonLevelDec.get())
    {
        //[UserButtonCode_buttonLevelDec] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::levelDec);
        //[/UserButtonCode_buttonLevelDec]
    }
    else if (buttonThatWasClicked == buttonLevelInc.get())
    {
        //[UserButtonCode_buttonLevelInc] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::levelInc);
        //[/UserButtonCode_buttonLevelInc]
    }
    else if (buttonThatWasClicked == buttonReverbDec.get())
    {
        //[UserButtonCode_buttonReverbDec] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::reverbDec);
        //[/UserButtonCode_buttonReverbDec]
    }
    else if (buttonThatWasClicked == buttonReverbInc.get())
    {
        //[UserButtonCode_buttonReverbInc] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::reverbInc);
        //[/UserButtonCode_buttonReverbInc]
    }
    else if (buttonThatWasClicked == buttonPartDec.get())
    {
        //[UserButtonCode_buttonPartDec] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::partDec);
        //[/UserButtonCode_buttonPartDec]
    }
    else if (buttonThatWasClicked == buttonPartInc.get())
    {
        //[UserButtonCode_buttonPartInc] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::partInc);
        //[/UserButtonCode_buttonPartInc]
    }
    else if (buttonThatWasClicked == buttonKeyShiftDec.get())
    {
        //[UserButtonCode_buttonKeyShiftDec] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::keyShiftDec);
        //[/UserButtonCode_buttonKeyShiftDec]
    }
    else if (buttonThatWasClicked == buttonKeyShiftInc.get())
    {
        //[UserButtonCode_buttonKeyShiftInc] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::keyShiftInc);
        //[/UserButtonCode_buttonKeyShiftInc]
    }
    else if (buttonThatWasClicked == buttonAll.get())
    {
        //[UserButtonCode_buttonAll] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::all);
        syncFrontPanelIndicators();
        //[/UserButtonCode_buttonAll]
    }
    else if (buttonThatWasClicked == buttonAll2.get())
    {
        //[UserButtonCode_buttonAll2] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::mute);
        syncFrontPanelIndicators();
        //[/UserButtonCode_buttonAll2]
    }
    else if (buttonThatWasClicked == buttonPanDec.get())
    {
        //[UserButtonCode_buttonPanDec] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::panDec);
        //[/UserButtonCode_buttonPanDec]
    }
    else if (buttonThatWasClicked == buttonPanInc.get())
    {
        //[UserButtonCode_buttonPanInc] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::panInc);
        //[/UserButtonCode_buttonPanInc]
    }
    else if (buttonThatWasClicked == buttonChorusDec.get())
    {
        //[UserButtonCode_buttonChorusDec] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::chorusDec);
        //[/UserButtonCode_buttonChorusDec]
    }
    else if (buttonThatWasClicked == buttonChorusInc.get())
    {
        //[UserButtonCode_buttonChorusInc] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::chorusInc);
        //[/UserButtonCode_buttonChorusInc]
    }
    else if (buttonThatWasClicked == buttonInstDec.get())
    {
        //[UserButtonCode_buttonInstDec] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::instrumentDec);
        //[/UserButtonCode_buttonInstDec]
    }
    else if (buttonThatWasClicked == buttonInstInc.get())
    {
        //[UserButtonCode_buttonInstInc] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::instrumentInc);
        //[/UserButtonCode_buttonInstInc]
    }
    else if (buttonThatWasClicked == buttonMidiChDec.get())
    {
        //[UserButtonCode_buttonMidiChDec] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::midiChannelDec);
        //[/UserButtonCode_buttonMidiChDec]
    }
    else if (buttonThatWasClicked == buttonMidiChInc.get())
    {
        //[UserButtonCode_buttonMidiChInc] -- add your button handler code here..
        audioProcessor.pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton::midiChannelInc);
        //[/UserButtonCode_buttonMidiChInc]
    }
    else if (buttonThatWasClicked == buttonPower.get())
    {
        //[UserButtonCode_buttonPower] -- add your button handler code here..
        audioProcessor.requestGsReset();
        //[/UserButtonCode_buttonPower]
    }
    else if (buttonThatWasClicked == button2x.get())
    {
        //[UserButtonCode_button2x] -- add your button handler code here..
        audioProcessor.setTwoXEnabled (button2x->getToggleState());
        syncFrontPanelIndicators();
        //[/UserButtonCode_button2x]
    }
    else if (buttonThatWasClicked == buttonPlayPause.get())
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

    //[UserbuttonClicked_Post]
    //[/UserbuttonClicked_Post]
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



//[MiscUserCode] You can add your own definitions of your custom methods or any other code here...

void NukedSC55AudioProcessorEditor::syncFrontPanelIndicators()
{
    const auto state = audioProcessor.getUiStatus().emulator;
    const auto applyIndicatorColour = [] (juce::TextButton* button, bool isLit)
    {
        if (button == nullptr)
            return;

        const auto colour = isLit ? juce::Colour (0xffff5f0f)
                                  : juce::Colours::black;
        button->setColour (juce::TextButton::buttonColourId, colour);
        button->setColour (juce::TextButton::buttonOnColourId, colour);
        button->setColour (juce::TextButton::textColourOffId, juce::Colours::black);
        button->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    };

    applyIndicatorColour (buttonAll.get(), state.allLed);
    applyIndicatorColour (buttonAll2.get(), state.muteLed);
    const auto twoXEnabled = audioProcessor.isTwoXEnabled();
    if (button2x != nullptr)
        button2x->setToggleState (twoXEnabled, juce::dontSendNotification);
    applyIndicatorColour (button2x.get(), twoXEnabled);
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

        if (! audioProcessor.loadMidiFile (file))
        {
            const auto options = juce::MessageBoxOptions::makeOptionsOk (
                juce::AlertWindow::WarningIcon, "SC-55",
                "このシーケンスファイルを再生できませんでした:\n" + file.getFileName());
            juce::AlertWindow::showAsync (options, nullptr);
        }

        syncPlaybackControls();

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
           mode="0"/>
  </BACKGROUND>
  <GENERICCOMPONENT name="" id="c4c91c74bed6da56" memberName="lcd" virtualName=""
                    explicitFocusOrder="0" pos="260 36 344 124" class="juce::Component"
                    params=""/>
  <TEXTBUTTON name="" id="791b54d63a59beec" memberName="buttonLevelDec" virtualName=""
              explicitFocusOrder="0" pos="768 67 52 20" buttonText="&lt;" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="b0fabff2cff12ab0" memberName="buttonLevelInc" virtualName=""
              explicitFocusOrder="0" pos="820 67 52 20" buttonText="&gt;" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="434dde801941ad4a" memberName="buttonReverbDec" virtualName=""
              explicitFocusOrder="0" pos="768 110 52 20" buttonText="&lt;"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="bd114b5368c9afc1" memberName="buttonReverbInc" virtualName=""
              explicitFocusOrder="0" pos="820 110 52 20" buttonText="&gt;"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="4e736adc3d682aca" memberName="buttonPartDec" virtualName=""
              explicitFocusOrder="0" pos="768 24 52 20" buttonText="&lt;" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="a8024d435ae254f2" memberName="buttonPartInc" virtualName=""
              explicitFocusOrder="0" pos="820 24 52 20" buttonText="&gt;" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="3dcc4df2d569df1d" memberName="buttonKeyShiftDec"
              virtualName="" explicitFocusOrder="0" pos="768 154 52 20" buttonText="&lt;"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="ef425c2d36a9db79" memberName="buttonKeyShiftInc"
              virtualName="" explicitFocusOrder="0" pos="820 154 52 20" buttonText="&gt;"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="25a57efa074a0574" memberName="buttonAll" virtualName=""
              explicitFocusOrder="0" pos="696 22 24 24" buttonText="" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="71f4c1e8344b8976" memberName="buttonAll2" virtualName=""
              explicitFocusOrder="0" pos="696 65 24 24" buttonText="" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="8feae8b2907afa7c" memberName="buttonPanDec" virtualName=""
              explicitFocusOrder="0" pos="894 67 52 20" buttonText="&lt;" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="c4a4f333bc4f6077" memberName="buttonPanInc" virtualName=""
              explicitFocusOrder="0" pos="946 67 52 20" buttonText="&gt;" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="3ce8ae39f180f7d6" memberName="buttonChorusDec" virtualName=""
              explicitFocusOrder="0" pos="894 110 52 20" buttonText="&lt;"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="fe30299cdb825a6e" memberName="buttonChorusInc" virtualName=""
              explicitFocusOrder="0" pos="946 110 52 20" buttonText="&gt;"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="2311080f88afd3be" memberName="buttonInstDec" virtualName=""
              explicitFocusOrder="0" pos="894 24 52 20" buttonText="&lt;" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="d2c4415527b1cc32" memberName="buttonInstInc" virtualName=""
              explicitFocusOrder="0" pos="946 24 52 20" buttonText="&gt;" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="bdf6a93618a72da4" memberName="buttonMidiChDec" virtualName=""
              explicitFocusOrder="0" pos="894 154 52 20" buttonText="&lt;"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="f023098fa71e0d7" memberName="buttonMidiChInc" virtualName=""
              explicitFocusOrder="0" pos="946 154 52 20" buttonText="&gt;"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="92aaef5f50d547a5" memberName="buttonPower" virtualName=""
              explicitFocusOrder="0" pos="24 24 72 24" buttonText="" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <SLIDER name="" id="56f744301daef134" memberName="sliderMasterVolume"
          virtualName="" explicitFocusOrder="0" pos="132 24 64 64" min="0.0"
          max="100.0" int="1.0" style="RotaryVerticalDrag" textBoxPos="NoTextBox"
          textBoxEditable="1" textBoxWidth="80" textBoxHeight="20" skewFactor="1.0"
          needsCallback="1" filmstripImage="BinaryData::Volume_png" filmstripFrames="101"
          filmstripVertical="1"/>
  <TEXTBUTTON name="" id="ae42fcf2a627eaf2" memberName="button2x" virtualName=""
              explicitFocusOrder="0" pos="696 108 24 24" buttonText="" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
  <LABEL name="" id="571536871ed7a09d" memberName="label2x" virtualName=""
         explicitFocusOrder="0" pos="648 112 46 16" edTextCol="ff000000"
         edBkgCol="0" labelText="2X" editableSingleClick="0" editableDoubleClick="0"
         focusDiscardsChanges="0" fontname="Default font" fontsize="15.0"
         kerning="0.0" bold="0" italic="0" justification="34"/>
  <TEXTBUTTON name="" id="d38aac467e703aaf" memberName="buttonPlayPause" virtualName=""
              explicitFocusOrder="0" pos="120 120 80 24" buttonText="PLAY"
              connectedEdges="0" needsCallback="1" radioGroupId="0"/>
  <TEXTBUTTON name="" id="ae0fc65259da31ea" memberName="buttonStop" virtualName=""
              explicitFocusOrder="0" pos="24 120 80 24" buttonText="STOP" connectedEdges="0"
              needsCallback="1" radioGroupId="0"/>
</JUCER_COMPONENT>

END_JUCER_METADATA
*/
#endif


//[EndFile] You can add extra defines here...
//[/EndFile]

