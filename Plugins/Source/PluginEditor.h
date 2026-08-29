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
                                       public juce::Button::Listener,
                                       public juce::Slider::Listener
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
    //[/UserMethods]

    void paint (juce::Graphics& g) override;
    void resized() override;
    void buttonClicked (juce::Button* buttonThatWasClicked) override;
    void sliderValueChanged (juce::Slider* sliderThatWasMoved) override;



private:
    //[UserVariables]   -- You can add your own custom variables in this section.
    // This reference is provided as a quick way for your editor to
    // access the processor object that created it.
    NukedSC55AudioProcessor& audioProcessor;
    std::unique_ptr<LcdDisplay> lcdDisplay;
    bool fileDragActive = false;
    //[/UserVariables]

    //==============================================================================
    juce::Component contentComponent;
    std::unique_ptr<juce::Component> lcd;
    std::unique_ptr<juce::Component> acrylPanel;
    std::unique_ptr<juce::TextButton> buttonLevelDec;
    std::unique_ptr<juce::TextButton> buttonLevelInc;
    std::unique_ptr<juce::TextButton> buttonReverbDec;
    std::unique_ptr<juce::TextButton> buttonReverbInc;
    std::unique_ptr<juce::TextButton> buttonPartDec;
    std::unique_ptr<juce::TextButton> buttonPartInc;
    std::unique_ptr<juce::TextButton> buttonKeyShiftDec;
    std::unique_ptr<juce::TextButton> buttonKeyShiftInc;
    std::unique_ptr<juce::TextButton> buttonAll;
    std::unique_ptr<juce::TextButton> buttonAll2;
    std::unique_ptr<juce::TextButton> buttonPanDec;
    std::unique_ptr<juce::TextButton> buttonPanInc;
    std::unique_ptr<juce::TextButton> buttonChorusDec;
    std::unique_ptr<juce::TextButton> buttonChorusInc;
    std::unique_ptr<juce::TextButton> buttonInstDec;
    std::unique_ptr<juce::TextButton> buttonInstInc;
    std::unique_ptr<juce::TextButton> buttonMidiChDec;
    std::unique_ptr<juce::TextButton> buttonMidiChInc;
    std::unique_ptr<juce::TextButton> buttonPower;
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
    juce::Image cachedImage_BinaryData_Background_png_2;


    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NukedSC55AudioProcessorEditor)
};

//[EndFile] You can add extra defines here...
//[/EndFile]

