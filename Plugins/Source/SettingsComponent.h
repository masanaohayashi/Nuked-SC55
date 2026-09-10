/*
  ==============================================================================

  This is an automatically generated GUI class created by the Projucer!

  Be careful when adding custom code to these files, as only the code within
  the "//[xyz]" and "//[/xyz]" sections will be retained when the file is loaded
  and re-saved.

  Created with Projucer version: 9.0.2

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

class ImportAwareComboBox final : public juce::ComboBox
{
public:
    using juce::ComboBox::ComboBox;

    void setImportOnly (bool shouldImportOnly) noexcept
    {
        importOnly = shouldImportOnly;
    }

    void setOnImportRequested (std::function<void()> callback)
    {
        onImportRequested = std::move (callback);
    }

    void showPopup() override
    {
        if (importOnly && onImportRequested)
        {
            DBG ("[DEBUG-SC55] ROM combo clicked with no stored ROM; opening import chooser");
            hidePopup();
            onImportRequested();
            return;
        }

        juce::ComboBox::showPopup();
    }

private:
    bool importOnly = false;
    std::function<void()> onImportRequested;
};
//[/Headers]



//==============================================================================
/**
                                                                    //[Comments]
    An auto-generated component, created by the Projucer.

    Describe your class and how it works here!
                                                                    //[/Comments]
*/
class SettingsComponent  : public juce::Component,
                           public juce::Button::Listener,
                           public juce::ComboBox::Listener,
                           public juce::Slider::Listener
{
public:
    //==============================================================================
    SettingsComponent ();
    ~SettingsComponent() override;

    //==============================================================================
    //[UserMethods]     -- You can add your own custom methods in this section.
    using Action = std::function<void()>;
    using RomSelectionAction = std::function<void(const juce::String&)>;

    void setOnClose (Action callback) { onClose = std::move (callback); }
    void setOnImportRom (Action callback)
    {
        onImportRom = std::move (callback);

        if (comboRoms != nullptr)
            comboRoms->setOnImportRequested ([this]
            {
                if (onImportRom)
                    onImportRom();
            });
    }
    void setOnRomSelected (RomSelectionAction callback) { onRomSelected = std::move (callback); }
    void setOnOptimizationChanged (std::function<void(bool)> callback)
    { onOptimizationChanged = std::move (callback); }
    void setOptimizationEnabled (bool enabled)
    { toggleOptimization->setToggleState (enabled, juce::dontSendNotification); }
    void setOptimizationAvailable (bool available)
    { toggleOptimization->setEnabled (available); }
    void setOnVoicesChanged (std::function<void(unsigned)> callback)
    { onVoicesChanged = std::move (callback); }
    void setVoiceLimit (unsigned voices, bool available, bool force = false)
    {
        sliderVoices->setEnabled (available);
        // Do not overwrite a pending asynchronous drag/text notification with
        // the unchanged processor value during the editor's meter refresh.
        if ((force || voices != shownVoiceLimit) && ! sliderVoices->isMouseButtonDown (true))
        {
            shownVoiceLimit = voices;
            sliderVoices->setValue (voices, juce::dontSendNotification);
        }
    }

    void setRomChoices (const juce::StringArray& names,
                       const juce::String& selectedName);
    void setSelectedRomName (const juce::String& name);
    void setAudioDeviceManager (juce::AudioDeviceManager* manager);

    //[/UserMethods]

    void paint (juce::Graphics& g) override;
    void resized() override;
    void buttonClicked (juce::Button* buttonThatWasClicked) override;
    void comboBoxChanged (juce::ComboBox* comboBoxThatHasChanged) override;
    void sliderValueChanged (juce::Slider* sliderThatWasMoved) override;



private:
    //[UserVariables]   -- You can add your own custom variables in this section.
    Action onClose;
    Action onImportRom;
    RomSelectionAction onRomSelected;
    std::function<void(bool)> onOptimizationChanged;
    std::function<void(unsigned)> onVoicesChanged;
    unsigned shownVoiceLimit = 0;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> audioDeviceSettings;
    juce::String selectedRomName;
    static constexpr int importRomItemId = 0x10000;
    //[/UserVariables]

    //==============================================================================
    juce::Component contentComponent;
    std::unique_ptr<juce::ImageButton> buttonClose;
    std::unique_ptr<juce::Label> labelCurrentRomCaption;
    std::unique_ptr<ImportAwareComboBox> comboRoms;
    std::unique_ptr<juce::Label> juce__label;
    std::unique_ptr<juce::Viewport> viewport;
    std::unique_ptr<juce::Label> labelVoices;
    std::unique_ptr<juce::Slider> sliderVoices;
    std::unique_ptr<juce::ToggleButton> toggleOptimization;
    juce::Image cachedImage_BinaryData_BackPanel_png_1;


    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsComponent)
};

//[EndFile] You can add extra defines here...
//[/EndFile]

