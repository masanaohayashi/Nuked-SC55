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

#pragma once

//[Headers]     -- You can add your own extra header files here --
#include <JuceHeader.h>
#include "PluginProcessor.h"
class LcdDisplay;
class SettingsComponent;
//[/Headers]



//==============================================================================
/**
                                                                    //[Comments]
    An auto-generated component, created by the Projucer.

    Describe your class and how it works here!
                                                                    //[/Comments]
*/
class NukedSC55AudioProcessorEditor  : public juce::AudioProcessorEditor,
                                       public juce::FileDragAndDropTarget,
                                       public juce::Slider::Listener,
                                       public juce::Button::Listener
{
public:
    //==============================================================================
    NukedSC55AudioProcessorEditor (NukedSC55AudioProcessor& p);
    ~NukedSC55AudioProcessorEditor() override;

    //==============================================================================
    //[UserMethods]     -- You can add your own custom methods in this section.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void syncFrontPanelIndicators();
    void syncPlaybackControls();
    void loadSequenceFile (const juce::File& file);
    void showSequenceFileChooser();
    void showRomFileChooser();
    juce::Image loadMakerLogoImage() const;
    void setMakerLogoImage (const juce::Image& image);
    void replaceMakerLogoFromFile (const juce::File& file);
    bool isPointOnMakerLogo (int x, int y);
    void setSettingsVisible (bool shouldBeVisible);
    void showStandaloneAudioSettings();
    //[/UserMethods]

    void paint (juce::Graphics& g) override;
    void resized() override;
    void sliderValueChanged (juce::Slider* sliderThatWasMoved) override;
    void buttonClicked (juce::Button* buttonThatWasClicked) override;



private:
    //[UserVariables]   -- You can add your own custom variables in this section.
    // This reference is provided as a quick way for your editor to
    // access the processor object that created it.
    NukedSC55AudioProcessor& audioProcessor;
    std::unique_ptr<LcdDisplay> lcdDisplay;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterVolumeAttachment;
    bool fileDragActive = false;
    std::unique_ptr<juce::FileChooser> sequenceFileChooser;
    std::unique_ptr<juce::FileChooser> romFileChooser;
    std::unique_ptr<SettingsComponent> settingsComponent;
    //[/UserVariables]

    //==============================================================================
    juce::Component contentComponent;
    std::unique_ptr<juce::Component> lcd;
    std::unique_ptr<juce::Slider> sliderMasterVolume;
    struct FilmstripSliderLookAndFeel1  : public juce::LookAndFeel_V4
    {
        void setFilmstrip (juce::Image imageToUse, int numFramesToUse, bool verticalLayoutToUse)
        {
            image = imageToUse;
            numFrames = juce::jmax (1, numFramesToUse);
            verticalLayout = verticalLayoutToUse;
        }

        void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                               float sliderPos, float rotaryStartAngle,
                               float rotaryEndAngle, juce::Slider& slider) override
        {
            if (image.isValid() && numFrames > 1)
            {
                auto frame = juce::jlimit (0, numFrames - 1,
                                          juce::roundToInt (sliderPos * (float) (numFrames - 1)));

                if (verticalLayout)
                {
                    auto frameHeight = image.getHeight() / numFrames;

                    if (frameHeight > 0)
                        g.drawImage (image, x, y, width, height,
                                     0, frame * frameHeight, image.getWidth(), frameHeight);
                }
                else
                {
                    auto frameWidth = image.getWidth() / numFrames;

                    if (frameWidth > 0)
                        g.drawImage (image, x, y, width, height,
                                     frame * frameWidth, 0, frameWidth, image.getHeight());
                }

                return;
            }

            juce::LookAndFeel_V4::drawRotarySlider (g, x, y, width, height, sliderPos,
                                                    rotaryStartAngle, rotaryEndAngle, slider);
        }

        juce::Image image;
        int numFrames = 1;
        bool verticalLayout = true;
    };

    FilmstripSliderLookAndFeel1 filmstripSliderLookAndFeel1;
    std::unique_ptr<juce::Label> label2x;
    std::unique_ptr<juce::TextButton> buttonPlayPause;
    std::unique_ptr<juce::TextButton> buttonStop;
    std::unique_ptr<juce::ImageButton> buttonPartDec2;
    std::unique_ptr<juce::ImageButton> buttonPartInc2;
    std::unique_ptr<juce::ImageButton> buttonInstDec2;
    std::unique_ptr<juce::ImageButton> buttonInstInc2;
    std::unique_ptr<r2juce::R2Led> ledPower;
    std::unique_ptr<juce::ImageButton> buttonMakerLogo;
    std::unique_ptr<juce::ImageButton> buttonSC;
    std::unique_ptr<juce::ImageButton> buttonMk2;
    std::unique_ptr<juce::ImageButton> buttonAll_new;
    std::unique_ptr<juce::ImageButton> buttonMute_new;
    std::unique_ptr<juce::ImageButton> button2x_new;
    std::unique_ptr<juce::ImageButton> buttonPower2;
    std::unique_ptr<juce::ImageButton> buttonLevelDec2;
    std::unique_ptr<juce::ImageButton> buttonLevelInc2;
    std::unique_ptr<juce::ImageButton> buttonPanDec2;
    std::unique_ptr<juce::ImageButton> buttonPanInc2;
    std::unique_ptr<juce::ImageButton> buttonReverbDec2;
    std::unique_ptr<juce::ImageButton> buttonReverbInc2;
    std::unique_ptr<juce::ImageButton> buttonChorusDec2;
    std::unique_ptr<juce::ImageButton> buttonChorusInc2;
    std::unique_ptr<juce::ImageButton> buttonKeyShiftDec2;
    std::unique_ptr<juce::ImageButton> buttonKeyShiftInc2;
    std::unique_ptr<juce::ImageButton> buttonMidiChDec2;
    std::unique_ptr<juce::ImageButton> buttonMidiChInc2;
    std::unique_ptr<juce::TextButton> buttonLoad;
    std::unique_ptr<juce::Label> labelPlayer;
    juce::Image cachedImage_BinaryData_Background_png_2;


    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NukedSC55AudioProcessorEditor)
};

//[EndFile] You can add extra defines here...
//[/EndFile]
