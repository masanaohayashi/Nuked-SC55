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
    //[/UserMethods]

    void paint (juce::Graphics& g) override;
    void resized() override;
    void buttonClicked (juce::Button* buttonThatWasClicked) override;



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
    juce::Image cachedImage_BinaryData_Background_png_1;


    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NukedSC55AudioProcessorEditor)
};

//[EndFile] You can add extra defines here...
//[/EndFile]

