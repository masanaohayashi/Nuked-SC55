/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ============================================================================
*/

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <JuceHeader.h>

#include "MidiFilePlayer.h"
#include "NukedSC55Emulator.h"

//==============================================================================
/**
*/
class NukedSC55AudioProcessor  : public juce::AudioProcessor,
                                 private juce::AsyncUpdater
{
public:
    struct UiStatus
    {
        bool audioReady = false;
        bool twoXEnabled = false;
        double sampleRate = 0.0;
        juce::String romDirectory;
        juce::String error;
        NukedSC55Emulator::DebugState emulator;
    };

    //==============================================================================
    NukedSC55AudioProcessor();
    ~NukedSC55AudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /** Requests the standalone app's first-run ROM selection dialog. */
    void requestRomSelection();

    /** Sends one momentary press through the SC-55's physical front-panel matrix. */
    void pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton button);

    /** Queues the Roland GS reset SysEx used by the SC-55 reset path. */
    void requestGsReset();

    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameters; }

    /** Enables the two-instance polyphony mode. */
    void setTwoXEnabled (bool enabled);
    bool isTwoXEnabled() const noexcept { return twoXEnabled.load (std::memory_order_acquire); }

    /** Plays a Standard MIDI File straight into the emulator, in file order. */
    bool startMidiFile (const juce::File& file);
    void stopMidiFile();
    bool isPlayingMidiFile() const noexcept { return midiFilePlaying.load (std::memory_order_acquire); }
    juce::String getMidiFileName() const { return midiFileName; }

    /** Returns a message-thread-readable snapshot for the editor LCD. */
    UiStatus getUiStatus() const;

    /** Copies the current SC-55 LCD segment mask into a row-major buffer. */
    bool copyLcdDisplay (uint8_t* destination, size_t destinationStride);

private:
    //==============================================================================
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void handleAsyncUpdate() override;
    bool initialiseRomDirectory (const juce::File& directory);
    void launchRomChooser();
    void sendMidiToEmulators (const uint8_t* data, int size) noexcept;

    juce::AudioProcessorValueTreeState parameters;
    std::array<NukedSC55Emulator, 2> emulators;
    juce::AudioBuffer<float> secondaryRenderBuffer;
    std::atomic<bool> audioReady { false };
    std::atomic<bool> twoXEnabled { false };
    std::atomic<bool> secondaryReleaseRequested { false };
    std::atomic<bool> romSelectionRequested { false };
    std::atomic<double> currentSampleRate { 0.0 };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterVolumeGain;

    // Accessed by the message-thread chooser and the message-thread editor.
    juce::String uiError;
    MidiFileData midiFile;
    std::atomic<bool> midiFilePlaying { false };
    double midiFilePosition = 0.0;
    size_t midiFileNext = 0;
    juce::String midiFileName;

    juce::File selectedRomDirectory;
    std::unique_ptr<juce::FileChooser> romChooser;
    std::shared_ptr<int> lifetimeToken { std::make_shared<int> (0) };
    uint64_t processBlockCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NukedSC55AudioProcessor)
};
