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
#include <functional>
#include <utility>
//[/Headers]



//==============================================================================
/**
                                                                    //[Comments]
    An auto-generated component, created by the Projucer.

    Describe your class and how it works here!
                                                                    //[/Comments]
*/
class SettingsComponent  : public juce::Component,
                           public juce::Button::Listener
{
public:
    //==============================================================================
    SettingsComponent ();
    ~SettingsComponent() override;

    //==============================================================================
    //[UserMethods]     -- You can add your own custom methods in this section.
    using Action = std::function<void()>;

    void setOnClose (Action callback) { onClose = std::move (callback); }
    void setOnAudioDeviceSettings (Action callback) { onAudioDeviceSettings = std::move (callback); }
    void setOnLoadRom (Action callback) { onLoadRom = std::move (callback); }
    void setOnGsReset (Action callback) { onGsReset = std::move (callback); }
    void setOnGmReset (Action callback) { onGmReset = std::move (callback); }

    void setAudioDeviceButtonEnabled (bool enabled)
    {
        if (buttonAudioDevice != nullptr)
            buttonAudioDevice->setEnabled (enabled);
    }

    void setCurrentRomDirectory (const juce::String& directory)
    {
        if (labelCurrentRom == nullptr)
            return;

        const auto displayName = directory.isEmpty()
                               ? juce::String ("n/a")
                               : juce::File (directory).getFileName();
        const auto text = displayName.isEmpty() ? directory : displayName;

        if (labelCurrentRom->getText() != text)
            labelCurrentRom->setText (text, juce::dontSendNotification);

        labelCurrentRom->setTooltip (directory);
    }
    //[/UserMethods]

    void paint (juce::Graphics& g) override;
    void resized() override;
    void buttonClicked (juce::Button* buttonThatWasClicked) override;



private:
    //[UserVariables]   -- You can add your own custom variables in this section.
    std::unique_ptr<juce::TextButton> buttonGsReset;
    std::unique_ptr<juce::TextButton> buttonGmReset;
    Action onClose;
    Action onAudioDeviceSettings;
    Action onLoadRom;
    Action onGsReset;
    Action onGmReset;
    //[/UserVariables]

    //==============================================================================
    juce::Component contentComponent;
    std::unique_ptr<juce::ImageButton> buttonClose;
    std::unique_ptr<juce::TextButton> buttonAudioDevice;
    std::unique_ptr<juce::Label> labelCurrentRom;
    std::unique_ptr<juce::Label> labelCurrentRomCaption;
    std::unique_ptr<juce::TextButton> buttonLoadRom;


    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsComponent)
};

//[EndFile] You can add extra defines here...
//[/EndFile]

