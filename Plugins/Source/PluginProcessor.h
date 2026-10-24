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
#include <vector>

#include <JuceHeader.h>

#include "StereoDipole.h"
#include "MidiFilePlayer.h"
#include "GS55Engine.h"

#if JUCE_STANDALONE_APPLICATION
class WrdDisplayWindow;
#endif

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
        double sampleRate = 0.0;
        juce::String romDirectory;
        juce::String error;
        GS55Engine::DebugState engine;
        bool hasNativeState = false;
        sc55::SynthState nativeState;
    };

    /**
        A message-thread-readable view of the immutable WRD frames and the
        audio clock.  The audio callback only publishes the numeric position;
        it never parses or renders WRD data.
    */
    struct WrdDisplayState
    {
        const MidiFileData* file = nullptr;
        double positionSeconds = 0.0;
        bool shouldBeVisible = false;
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

    /** Imports a ROM set from the URL returned by JUCE's native file chooser. */
    bool loadRomSelection (const juce::URL& selection);

    /** Returns the valid ROM folders imported into the shared ROM library. */
    juce::StringArray getStoredRomNames() const;

    /** Selects one of the ROM folders returned by getStoredRomNames(). */
    bool selectStoredRom (const juce::String& name);

    void setDipoleEnabled (bool enabled);
    bool isDipoleEnabled() const noexcept { return dipoleEnabled.load (std::memory_order_relaxed); }
    bool setMaximumVoices (unsigned voices);
    unsigned getMaximumVoices() const noexcept { return maximumVoices.load (std::memory_order_relaxed); }

    /** Returns the persistent user data directory (App Group on Apple platforms). */
    static juce::File getUserSettingsDirectory();

    /** Returns the shared directory containing imported ROM folders. */
    static juce::File getRomStorageDirectory();

    /** Sends one momentary press through the SC-55's physical front-panel matrix. */
    void pressFrontPanelButton (GS55Engine::FrontPanelButton button);

    /** Queues the Roland GS reset SysEx used by the SC-55 reset path. */
    void requestGsReset();

    /** Queues the standard MIDI GM System On reset SysEx. */
    void requestGmReset();

    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameters; }

    /** Loads a Standard MIDI File or RCP sequence without starting playback. */
    bool loadMidiFile (const juce::File& file);

    // Playlist operations are message-thread only, like loadMidiFile().
    const juce::Array<juce::File>& getPlaylist() const noexcept { return playlist; }
    int getPlaylistIndex() const noexcept { return playlistIndex; }
    void appendPlaylistFiles (const juce::Array<juce::File>& files);
    bool selectPlaylistTrack (int index);

    /** Loads a sequence and starts it immediately for API callers that need it. */
    bool startMidiFile (const juce::File& file);

    /** Starts or resumes the loaded sequence. */
    void playMidiFile();

    /** Pauses the loaded sequence and releases all sounding notes. */
    void pauseMidiFile();

    /** Stops the loaded sequence, resets controllers, and rewinds it. */
    void stopMidiFile();
    bool hasMidiFile() const noexcept { return midiFileLoaded.load (std::memory_order_acquire); }
    bool isPlayingMidiFile() const noexcept { return midiFilePlaying.load (std::memory_order_acquire); }
    juce::String getMidiFileName() const { return midiFileName; }

    /** Returns a message-thread-readable snapshot for the editor LCD. */
    UiStatus getUiStatus() const;

    /** Peak of JUCE's smoothed callback load, retained while the editor is closed. */
    double getMaximumProcessLoadPercent() const noexcept
    {
        return processLoadResetRequested.load() ? 0.0 : maximumProcessLoadPercent.load();
    }
    void resetMaximumProcessLoad() noexcept { processLoadResetRequested.store (true); }

    /** Message thread: the LCD's current contents. */
    bool getLcdFrame (GS55Engine::LcdFrame& destination) const { return engine.getLcdFrame (destination); }
    /** Message thread: the private SysEx panel image, when one is shown. */
    bool copyPanelImage (uint8_t* destination, size_t stride, bool* contentChanged, bool forceCopy) const
    { return engine.copyPanelRaster (destination, stride, contentChanged, forceCopy); }

    /** Returns the immutable WRD data and audio-clock snapshot for the window. */
    WrdDisplayState getWrdDisplayState() const noexcept;

private:
    //==============================================================================
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void handleAsyncUpdate() override;
    bool initialiseRomDirectory (const juce::File& directory);
    bool selectStoredRomInternal (const juce::String& name, bool notifyHost);
    void notifyRomSelectionChanged();
    void launchRomChooser();
    void sendMidiToEmulator (const uint8_t* data, int size) noexcept;
    void processMidiPlaybackCommands() noexcept;
    void sendAllNotesOff() noexcept;
    void sendResetAllControllers() noexcept;

    juce::AudioProcessorValueTreeState parameters;
    GS55Engine engine;
    std::atomic<unsigned> maximumVoices { 24 };
    std::atomic<bool> audioReady { false };
    std::atomic<bool> romSelectionRequested { false };
    std::atomic<double> currentSampleRate { 0.0 };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> masterVolumeGain;

    // Accessed by the message-thread chooser and the message-thread editor.
    juce::String uiError;
    // Parsed files are immutable after publication. The message thread owns
    // every allocation for the lifetime of the processor; the audio thread
    // only follows the published raw pointer and never frees file data.
    std::vector<std::unique_ptr<MidiFileData>> midiFileStorage;
    std::atomic<MidiFileData*> pendingMidiFile { nullptr };
    MidiFileData* activeMidiFile = nullptr;
    std::atomic<bool> midiFileLoaded { false };
    std::atomic<bool> midiFilePlaying { false };
    std::atomic<uint32_t> midiPlaybackCommands { 0 };
    std::atomic<double> midiFilePositionForUi { 0.0 };
    std::atomic<MidiFileData*> wrdFileForUi { nullptr };
    std::atomic<bool> wrdDisplayShouldBeVisible { false };
    double midiFilePosition = 0.0;
    size_t midiFileNext = 0;
    juce::String midiFileName;
    juce::Array<juce::File> playlist;
    int playlistIndex = -1;

#if JUCE_STANDALONE_APPLICATION
    std::unique_ptr<WrdDisplayWindow> wrdDisplayWindow;
#endif

    juce::File selectedRomDirectory;
    std::unique_ptr<juce::FileChooser> romChooser;
    std::shared_ptr<int> lifetimeToken { std::make_shared<int> (0) };
    uint64_t processBlockCount = 0;
    std::atomic<bool> dipoleEnabled { false };
    sc55dipole::Processor stereoDipole;

    // Only prepareToPlay/processBlock access the measurer; the UI uses atomics.
    juce::AudioProcessLoadMeasurer processLoadMeasurer;
    std::atomic<double> maximumProcessLoadPercent { 0.0 };
    std::atomic<bool> processLoadResetRequested { false };
    static_assert (std::atomic<double>::is_always_lock_free);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NukedSC55AudioProcessor)
};
