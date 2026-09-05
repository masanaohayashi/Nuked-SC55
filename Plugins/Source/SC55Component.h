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
//[/Headers]



//==============================================================================
/**
                                                                    //[Comments]
    An auto-generated component, created by the Projucer.

    Describe your class and how it works here!
                                                                    //[/Comments]
*/
class SC55Component  : public juce::Component,
                       public juce::Slider::Listener,
                       public juce::Button::Listener
{
public:
    //==============================================================================
    SC55Component ();
    ~SC55Component() override;

    //==============================================================================
    //[UserMethods]     -- You can add your own custom methods in this section.
    //[/UserMethods]

    void paint (juce::Graphics& g) override;
    void resized() override;
    void sliderValueChanged (juce::Slider* sliderThatWasMoved) override;
    void buttonClicked (juce::Button* buttonThatWasClicked) override;



private:
    //[UserVariables]   -- You can add your own custom variables in this section.
    //[/UserVariables]

    //==============================================================================
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
    std::unique_ptr<juce::ImageButton> buttonPartDec;
    std::unique_ptr<juce::ImageButton> buttonPartInc;
    std::unique_ptr<juce::ImageButton> buttonInstDec;
    std::unique_ptr<juce::ImageButton> buttonInstInc;
    std::unique_ptr<r2juce::R2Led> ledPower;
    std::unique_ptr<juce::ImageButton> buttonMakerLogo;
    std::unique_ptr<juce::ImageButton> buttonSC;
    std::unique_ptr<juce::ImageButton> buttonMk2;
    std::unique_ptr<juce::ImageButton> buttonAll;
    std::unique_ptr<juce::ImageButton> buttonMute;
    std::unique_ptr<juce::ImageButton> button2x;
    std::unique_ptr<juce::ImageButton> buttonPower;
    std::unique_ptr<juce::ImageButton> buttonLevelDec;
    std::unique_ptr<juce::ImageButton> buttonLevelInc;
    std::unique_ptr<juce::ImageButton> buttonPanDec;
    std::unique_ptr<juce::ImageButton> buttonPanInc;
    std::unique_ptr<juce::ImageButton> buttonReverbDec;
    std::unique_ptr<juce::ImageButton> buttonReverbInc;
    std::unique_ptr<juce::ImageButton> buttonChorusDec;
    std::unique_ptr<juce::ImageButton> buttonChorusInc;
    std::unique_ptr<juce::ImageButton> buttonKeyShiftDec;
    std::unique_ptr<juce::ImageButton> buttonKeyShiftInc;
    std::unique_ptr<juce::ImageButton> buttonMidiChDec;
    std::unique_ptr<juce::ImageButton> buttonMidiChInc;
    std::unique_ptr<juce::TextButton> buttonLoad;
    std::unique_ptr<juce::Label> labelPlayer;
    std::unique_ptr<juce::ImageButton> buttonGM;
    std::unique_ptr<juce::ImageButton> buttonGS;
    juce::Image cachedImage_BinaryData_Background_png_2;


    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SC55Component)
};

//[EndFile] You can add extra defines here...
//[/EndFile]

