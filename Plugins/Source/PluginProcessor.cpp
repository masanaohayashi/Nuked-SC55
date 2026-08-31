/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SC55Debug.h"

#if JUCE_STANDALONE_APPLICATION
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
 #include "WrdDisplayWindow.h"
#endif

#include <algorithm>
#include <cstdlib>

namespace
{
const char* const romDirectoryEnvironmentVariable = "NUKED_SC55_ROM_PATH";
constexpr uint8_t gsResetMessage[] =
    { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
constexpr uint32_t midiPauseCommand = 1u << 0;
constexpr uint32_t midiStopCommand = 1u << 1;

bool containsRomSet (const juce::File& directory)
{
    return directory.isDirectory()
        && NukedSC55Emulator::hasRomSet (directory.getFullPathName().toStdString());
}

juce::File getRememberedRomPathFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support")
        .getChildFile ("STUDIO-R")
        .getChildFile ("Nuked-SC55")
        .getChildFile ("rom-path.txt");
}

juce::File loadRememberedRomDirectory()
{
    const auto pathFile = getRememberedRomPathFile();
    if (! pathFile.existsAsFile())
        return {};

    const auto path = pathFile.loadFileAsString().trim();
    return path.isEmpty() ? juce::File() : juce::File (path);
}

void rememberRomDirectory (const juce::File& directory)
{
    const auto pathFile = getRememberedRomPathFile();
    pathFile.getParentDirectory().createDirectory();
    pathFile.replaceWithText (directory.getFullPathName(), false, false, "\n");
}

juce::File findRomDirectory()
{
    if (const auto* environmentPath = std::getenv (romDirectoryEnvironmentVariable);
        environmentPath != nullptr && *environmentPath != '\0')
    {
        juce::File environmentDirectory { juce::String (environmentPath) };
        if (containsRomSet (environmentDirectory))
            return environmentDirectory;
    }

    const auto rememberedDirectory = loadRememberedRomDirectory();
    if (containsRomSet (rememberedDirectory))
        return rememberedDirectory;

    const auto executableDirectory = juce::File::getSpecialLocation
        (juce::File::currentExecutableFile).getParentDirectory();

    juce::Array<juce::File> candidates;
    auto candidate = executableDirectory;
    for (int i = 0; i < 5; ++i)
    {
        candidates.addIfNotAlreadyThere (candidate);

        // A sandboxed AUv3 extension can read its own bundle and nothing else,
        // so a ROM set placed in Resources is the only one it can ever find.
        candidates.addIfNotAlreadyThere (candidate.getChildFile ("Resources"));
        candidates.addIfNotAlreadyThere (candidate.getChildFile ("Contents/Resources"));

        candidate = candidate.getParentDirectory();
    }
    candidates.addIfNotAlreadyThere (juce::File::getCurrentWorkingDirectory());

    // Inside the AUv3 sandbox this resolves to the extension's own container,
    // which the user can populate through Finder; outside it, it is ~/Documents.
    candidates.addIfNotAlreadyThere (juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));

    for (const auto& directory : candidates)
    {
        if (containsRomSet (directory))
            return directory;
    }

    return {};
}

juce::File findRomChooserDirectory()
{
    const auto rememberedDirectory = loadRememberedRomDirectory();
    if (rememberedDirectory.isDirectory())
        return rememberedDirectory;

    const auto executableDirectory = juce::File::getSpecialLocation
        (juce::File::currentExecutableFile).getParentDirectory();
    if (executableDirectory.isDirectory())
        return executableDirectory;

    return juce::File::getCurrentWorkingDirectory();
}

void logStandaloneAudioDeviceState (const char* reason)
{
#if JUCE_STANDALONE_APPLICATION
    if (auto* holder = juce::StandalonePluginHolder::getInstance(); holder != nullptr)
    {
        if (auto* device = holder->deviceManager.getCurrentAudioDevice(); device != nullptr)
        {
            const auto activeInputs = device->getActiveInputChannels();
            const auto activeOutputs = device->getActiveOutputChannels();
            const auto setup = holder->deviceManager.getAudioDeviceSetup();
            sc55debug::log (
                "standalone audio state reason=%s device=\"%s\" type=\"%s\" open=%d playing=%d "
                "rate=%.2f block=%d activeIn=%d activeOut=%d setupOut=%d defaultOut=%d "
                "cpu=%.3f outputDevice=\"%s\" error=\"%s\"",
                reason,
                device->getName().toRawUTF8(),
                device->getTypeName().toRawUTF8(),
                device->isOpen() ? 1 : 0,
                device->isPlaying() ? 1 : 0,
                device->getCurrentSampleRate(),
                device->getCurrentBufferSizeSamples(),
                activeInputs.countNumberOfSetBits(),
                activeOutputs.countNumberOfSetBits(),
                setup.outputChannels.countNumberOfSetBits(),
                setup.useDefaultOutputChannels ? 1 : 0,
                holder->deviceManager.getCpuUsage(),
                setup.outputDeviceName.toRawUTF8(),
                device->getLastError().toRawUTF8());
            return;
        }

        sc55debug::log ("standalone audio state reason=%s device=null", reason);
        return;
    }

    sc55debug::log ("standalone audio state reason=%s holder=null", reason);
#else
    juce::ignoreUnused (reason);
#endif
}
}

//==============================================================================
NukedSC55AudioProcessor::NukedSC55AudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
     , parameters (*this, nullptr, "PARAMETERS", createParameterLayout())
#else
     : parameters (*this, nullptr, "PARAMETERS", createParameterLayout())
#endif
{
    sc55debug::log ("processor constructed wrapper=%d acceptsMidi=%d",
                    wrapperType, acceptsMidi() ? 1 : 0);
}

juce::AudioProcessorValueTreeState::ParameterLayout NukedSC55AudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "masterVolume", 1 }, "Master Volume", 0, 100, 100));
    return layout;
}

NukedSC55AudioProcessor::~NukedSC55AudioProcessor()
{
    sc55debug::log ("processor destroyed");
    cancelPendingUpdate();
    wrdDisplayShouldBeVisible.store (false, std::memory_order_release);
    wrdFileForUi.store (nullptr, std::memory_order_release);
#if JUCE_STANDALONE_APPLICATION
    wrdDisplayWindow.reset();
#endif
    lifetimeToken.reset();
    romChooser.reset();
    audioReady.store (false, std::memory_order_release);
}

//==============================================================================
const juce::String NukedSC55AudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool NukedSC55AudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool NukedSC55AudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool NukedSC55AudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double NukedSC55AudioProcessor::getTailLengthSeconds() const
{
    return 2.0;
}

int NukedSC55AudioProcessor::getNumPrograms()
{
    return 1;
}

int NukedSC55AudioProcessor::getCurrentProgram()
{
    return 0;
}

void NukedSC55AudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String NukedSC55AudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void NukedSC55AudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void NukedSC55AudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sc55debug::log ("prepareToPlay rate=%.2f block=%d", sampleRate, samplesPerBlock);
    secondaryRenderBuffer.setSize (2, std::max (1, samplesPerBlock), false, true, true);
    currentSampleRate.store (sampleRate, std::memory_order_release);
    midiFilePlaying.store (false, std::memory_order_release);
    midiFilePositionForUi.store (0.0, std::memory_order_release);
    midiPlaybackCommands.fetch_or (midiStopCommand, std::memory_order_release);

    masterVolumeGain.reset (sampleRate, 0.01);
    const auto* masterVolume = parameters.getRawParameterValue ("masterVolume");
    const auto normalizedVolume = masterVolume != nullptr
        ? juce::jlimit (0.0f, 1.0f, masterVolume->load (std::memory_order_relaxed) / 100.0f)
        : 1.0f;
    masterVolumeGain.setCurrentAndTargetValue (normalizedVolume * normalizedVolume);

    triggerAsyncUpdate();
}

NukedSC55AudioProcessor::UiStatus NukedSC55AudioProcessor::getUiStatus() const
{
    UiStatus status;
    status.audioReady = audioReady.load (std::memory_order_acquire);
    status.twoXEnabled = twoXEnabled.load (std::memory_order_acquire);
    status.sampleRate = currentSampleRate.load (std::memory_order_acquire);
    status.romDirectory = selectedRomDirectory.getFullPathName();
    status.error = uiError;
    status.emulator = emulators[0].getDebugState();
    return status;
}

NukedSC55AudioProcessor::WrdDisplayState NukedSC55AudioProcessor::getWrdDisplayState() const noexcept
{
    WrdDisplayState state;
    state.file = wrdFileForUi.load (std::memory_order_acquire);
    state.positionSeconds = midiFilePositionForUi.load (std::memory_order_acquire);
    state.shouldBeVisible = wrdDisplayShouldBeVisible.load (std::memory_order_acquire);
    return state;
}

bool NukedSC55AudioProcessor::copyLcdDisplay (uint8_t* destination, size_t destinationStride)
{
    if (twoXEnabled.load (std::memory_order_acquire))
        return emulators[0].copyMergedLcdDisplay (emulators[1], destination, destinationStride);

    return emulators[0].copyLcdDisplay (destination, destinationStride);
}

void NukedSC55AudioProcessor::releaseResources()
{
    sc55debug::log ("releaseResources");

    const juce::ScopedLock callbackLock (getCallbackLock());
    const bool wasReady = audioReady.load (std::memory_order_acquire);
    audioReady.store (false, std::memory_order_release);
    secondaryReleaseRequested.store (false, std::memory_order_release);
    emulators[0].release();
    emulators[1].release();

    // Keep MIDI received while no ROM is selected, but discard bytes belonging
    // to an already-running instance when the host tears that instance down.
    if (wasReady)
    {
        emulators[0].clearPendingMidi();
        emulators[1].clearPendingMidi();
    }
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool NukedSC55AudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

void NukedSC55AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                             juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    ++processBlockCount;
    const auto numSamples = buffer.getNumSamples();
    const auto numChannels = buffer.getNumChannels();
    const int midiEventCount = midiMessages.getNumEvents();
    const bool shouldLogBlock = sc55debug::enabled()
                             && (processBlockCount <= 10
                                 || processBlockCount % 1000 == 0);

    if (shouldLogBlock)
    {
        sc55debug::log ("processBlock #%llu samples=%d channels=%d midiEvents=%d ready=%d emuReady=%d",
                        static_cast<unsigned long long> (processBlockCount), numSamples,
                        numChannels, midiEventCount,
                        audioReady.load (std::memory_order_acquire) ? 1 : 0,
                        emulators[0].isReady() ? 1 : 0);
    }

    for (int channel = 0; channel < numChannels; ++channel)
        buffer.clear (channel, 0, numSamples);

    const bool ready = audioReady.load (std::memory_order_acquire);
    auto* left = numChannels > 0 ? buffer.getWritePointer (0) : nullptr;
    auto* right = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;
    int renderedSamples = 0;

    processMidiPlaybackCommands();
    const bool playMidiFile = midiFilePlaying.load (std::memory_order_relaxed);
    const auto rate = currentSampleRate.load (std::memory_order_relaxed);
    const bool renderTwoX = twoXEnabled.load (std::memory_order_acquire)
                         && secondaryRenderBuffer.getNumSamples() > 0;

    if (secondaryReleaseRequested.exchange (false, std::memory_order_acq_rel))
    {
        for (int channel = 0; channel < 16; ++channel)
        {
            const uint8_t allOff[3] = { static_cast<uint8_t> (0xb0 | channel), 123, 0 };
            emulators[1].sendMidi (allOff, 3);
        }
    }

    // An SMF event used to be dispatched only at the beginning of the host
    // block. Keep the audio callback as the clock, but visit the file player at
    // roughly 1 ms intervals so large host blocks cannot quantise note starts by
    // 10 ms or more. Integer sample counts make the actual interval just under
    // 1 ms at common rates (44 samples at 44.1 kHz).
    const int midiFileQuantumSamples = playMidiFile && rate > 0.0
        ? std::max (1, static_cast<int> (rate * 0.001))
        : numSamples;

    auto dispatchMidiFileEvents = [&]() noexcept
    {
        if (! playMidiFile || activeMidiFile == nullptr)
            return;

        // File order is preserved exactly; nothing here sorts or merges events.
        while (midiFileNext < activeMidiFile->events.size()
               && activeMidiFile->events[midiFileNext].seconds <= midiFilePosition)
        {
            const auto& e = activeMidiFile->events[midiFileNext++];
            sendMidiToEmulators (e.bytes.data(), static_cast<int> (e.bytes.size()));
        }
    };

    auto advanceMidiFileClock = [&] (int samples) noexcept
    {
        if (playMidiFile && rate > 0.0)
            midiFilePosition += static_cast<double> (samples) / rate;
    };

    auto hostMidiIterator = midiMessages.cbegin();
    const auto hostMidiEnd = midiMessages.cend();
    bool hasHostMidi = hostMidiIterator != hostMidiEnd;
    juce::MidiMessageMetadata nextHostMidi {};
    int nextHostMidiPosition = 0;

    if (hasHostMidi)
    {
        nextHostMidi = *hostMidiIterator;
        nextHostMidiPosition = juce::jlimit (0, numSamples, nextHostMidi.samplePosition);
    }

    auto dispatchHostMidiEvents = [&]() noexcept
    {
        while (hasHostMidi && nextHostMidiPosition <= renderedSamples)
        {
            const auto message = nextHostMidi.getMessage();
            sendMidiToEmulators (message.getRawData(), message.getRawDataSize());

            ++hostMidiIterator;
            hasHostMidi = hostMidiIterator != hostMidiEnd;
            if (hasHostMidi)
            {
                nextHostMidi = *hostMidiIterator;
                nextHostMidiPosition = juce::jlimit (0, numSamples, nextHostMidi.samplePosition);
            }
        }
    };

    while (renderedSamples < numSamples)
    {
        dispatchMidiFileEvents();
        dispatchHostMidiEvents();

        int nextRenderPosition = numSamples;
        if (playMidiFile)
            nextRenderPosition = std::min (nextRenderPosition,
                                           renderedSamples + midiFileQuantumSamples);
        if (renderTwoX)
            nextRenderPosition = std::min (nextRenderPosition,
                                           renderedSamples + secondaryRenderBuffer.getNumSamples());
        if (hasHostMidi)
            nextRenderPosition = std::min (nextRenderPosition, nextHostMidiPosition);

        const int segmentSamples = nextRenderPosition - renderedSamples;
        if (ready && left != nullptr && segmentSamples > 0)
        {
            emulators[0].render (left + renderedSamples,
                                 right != nullptr ? right + renderedSamples : nullptr,
                                 segmentSamples);

            if (renderTwoX)
            {
                auto* secondaryLeft = secondaryRenderBuffer.getWritePointer (0);
                auto* secondaryRight = right != nullptr
                                     ? secondaryRenderBuffer.getWritePointer (1)
                                     : nullptr;
                emulators[1].render (secondaryLeft, secondaryRight, segmentSamples);

                for (int i = 0; i < segmentSamples; ++i)
                {
                    left[renderedSamples + i] += secondaryLeft[i];
                    if (right != nullptr)
                        right[renderedSamples + i] += secondaryRight[i];
                }
            }
        }

        renderedSamples = nextRenderPosition;
        advanceMidiFileClock (segmentSamples);
    }

    // Events exactly at the end of a host block are queued now and consumed
    // when the next audio segment starts, matching JUCE's MIDI semantics.
    dispatchMidiFileEvents();
    dispatchHostMidiEvents();

    if (playMidiFile && activeMidiFile != nullptr
        && midiFilePosition > activeMidiFile->totalSeconds())
    {
        // Natural completion has the same rewind semantics as Stop.  Keep the
        // playback clock owned by the audio thread so the next Play starts at
        // the first event without a cross-thread position reset.
        midiFilePlaying.store (false, std::memory_order_release);
        midiFilePosition = 0.0;
        midiFileNext = 0;
    }

    // This is the only WRD-related operation in the audio callback: publish a
    // scalar clock for the standalone window.  WRD frame selection, parsing,
    // string conversion, and painting all happen on the message thread.
    midiFilePositionForUi.store (midiFilePosition, std::memory_order_release);

    const auto* masterVolume = parameters.getRawParameterValue ("masterVolume");
    const auto normalizedVolume = masterVolume != nullptr
        ? juce::jlimit (0.0f, 1.0f, masterVolume->load (std::memory_order_relaxed) / 100.0f)
        : 1.0f;

    // Apply the squared amplitude curve to the final mixed output. The target
    // is smoothed in linear-gain space one sample at a time so slider moves
    // and host automation cannot create block-rate zipper noise.
    masterVolumeGain.setTargetValue (normalizedVolume * normalizedVolume);
    float* const* outputChannels = buffer.getArrayOfWritePointers();
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto gain = masterVolumeGain.getNextValue();
        for (int channel = 0; channel < numChannels; ++channel)
            outputChannels[channel][sample] *= gain;
    }

    if ((! ready || left == nullptr || numSamples <= 0) && shouldLogBlock)
        sc55debug::log ("processBlock #%llu returned silent before render",
                        static_cast<unsigned long long> (processBlockCount));

    if (shouldLogBlock)
    {
        float outputPeak = 0.0f;
        for (int channel = 0; channel < numChannels; ++channel)
            outputPeak = std::max (outputPeak, buffer.getMagnitude (channel, 0, numSamples));
        sc55debug::log ("processBlock #%llu rendered=%d outputPeak=%.7f",
                        static_cast<unsigned long long> (processBlockCount),
                        numSamples, outputPeak);
    }
}

bool NukedSC55AudioProcessor::loadMidiFile (const juce::File& file)
{
    MidiFileData loaded;
    std::string error;
    const bool loadWrd = wrapperType == wrapperType_Standalone;
    if (! loaded.load (file.getFullPathName().toStdString(), error, loadWrd))
    {
        sc55debug::log ("MIDI file rejected: %s", error.c_str());
        return false;
    }

    sc55debug::log ("MIDI file loaded: %s (%zu events, %.1f s)",
                    file.getFileName().toRawUTF8(), loaded.events.size(), loaded.totalSeconds());

    auto fileData = std::make_unique<MidiFileData> (std::move (loaded));
    auto* publishedFile = fileData.get();
    midiFileStorage.push_back (std::move (fileData));

    const auto wrdFrameCount = publishedFile->wrdFrames.size();
    if (! publishedFile->wrdParseError.empty())
        sc55debug::log ("WRD companion ignored: %s", publishedFile->wrdParseError.c_str());

    midiFileName = file.getFileName();
    midiFilePlaying.store (false, std::memory_order_release);
    midiFilePositionForUi.store (0.0, std::memory_order_release);
    wrdFileForUi.store (wrdFrameCount > 0 ? publishedFile : nullptr,
                        std::memory_order_release);
    wrdDisplayShouldBeVisible.store (wrdFrameCount > 0,
                                     std::memory_order_release);
    midiFileLoaded.store (true, std::memory_order_release);
    pendingMidiFile.store (publishedFile, std::memory_order_release);

#if JUCE_STANDALONE_APPLICATION
    if (wrapperType == wrapperType_Standalone)
    {
        if (wrdDisplayWindow == nullptr)
            wrdDisplayWindow = std::make_unique<WrdDisplayWindow> (*this);

        if (wrdFrameCount > 0)
            wrdDisplayWindow->showForFile (file.getFileName());
        else
            wrdDisplayWindow->hideForPlaybackStop();
    }
#endif
    return true;
}

bool NukedSC55AudioProcessor::startMidiFile (const juce::File& file)
{
    if (! loadMidiFile (file))
        return false;

    playMidiFile();
    return true;
}

void NukedSC55AudioProcessor::playMidiFile()
{
    if (! hasMidiFile())
        return;

    midiFilePlaying.store (true, std::memory_order_release);
}

void NukedSC55AudioProcessor::pauseMidiFile()
{
    if (! hasMidiFile())
        return;

    midiFilePlaying.store (false, std::memory_order_release);
    midiPlaybackCommands.fetch_or (midiPauseCommand, std::memory_order_release);
}

void NukedSC55AudioProcessor::stopMidiFile()
{
    if (! hasMidiFile())
        return;

    midiFilePlaying.store (false, std::memory_order_release);
    midiFilePositionForUi.store (0.0, std::memory_order_release);
    wrdDisplayShouldBeVisible.store (false, std::memory_order_release);
    midiPlaybackCommands.fetch_or (midiStopCommand, std::memory_order_release);
}

void NukedSC55AudioProcessor::processMidiPlaybackCommands() noexcept
{
    if (auto* pendingFile = pendingMidiFile.exchange (nullptr, std::memory_order_acquire);
        pendingFile != nullptr)
    {
        activeMidiFile = pendingFile;
        midiFilePosition = 0.0;
        midiFileNext = 0;
        midiFilePositionForUi.store (0.0, std::memory_order_release);

        // Replacing a sequence must not leave notes or controller state from
        // the previous sequence in the emulated instrument.
        sendAllNotesOff();
        sendResetAllControllers();
    }

    const auto commands = midiPlaybackCommands.exchange (0, std::memory_order_acq_rel);
    if ((commands & midiPauseCommand) != 0)
        sendAllNotesOff();

    if ((commands & midiStopCommand) != 0)
    {
        sendAllNotesOff();
        sendResetAllControllers();
        midiFilePosition = 0.0;
        midiFileNext = 0;
        midiFilePositionForUi.store (0.0, std::memory_order_release);
    }
}

void NukedSC55AudioProcessor::sendAllNotesOff() noexcept
{
    for (int channel = 0; channel < 16; ++channel)
    {
        const uint8_t allOff[3] = { static_cast<uint8_t> (0xb0 | channel), 123, 0 };
        sendMidiToEmulators (allOff, 3);
    }
}

void NukedSC55AudioProcessor::sendResetAllControllers() noexcept
{
    for (int channel = 0; channel < 16; ++channel)
    {
        const uint8_t reset[3] = { static_cast<uint8_t> (0xb0 | channel), 121, 0 };
        sendMidiToEmulators (reset, 3);
    }
}


void NukedSC55AudioProcessor::requestRomSelection()
{
    if (wrapperType != wrapperType_Standalone)
    {
        sc55debug::log ("ROM chooser request ignored for wrapper=%d", wrapperType);
        return;
    }

    sc55debug::log ("ROM chooser requested");
    uiError.clear();
    romSelectionRequested.store (true, std::memory_order_release);
    triggerAsyncUpdate();
}

void NukedSC55AudioProcessor::pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton button)
{
    emulators[0].pressFrontPanelButton (button);
    if (twoXEnabled.load (std::memory_order_acquire))
        emulators[1].pressFrontPanelButton (button);
}

void NukedSC55AudioProcessor::requestGsReset()
{
    sc55debug::log ("GS reset requested by power button");
    sendMidiToEmulators (gsResetMessage, static_cast<int> (sizeof (gsResetMessage)));
}

void NukedSC55AudioProcessor::setTwoXEnabled (bool enabled)
{
    if (enabled
        && audioReady.load (std::memory_order_acquire)
        && ! emulators[1].isReady())
    {
        sc55debug::log ("2X mode unavailable: secondary emulator is not ready");
        enabled = false;
    }

    const bool previous = twoXEnabled.exchange (enabled, std::memory_order_acq_rel);
    if (previous == enabled)
        return;

    if (previous && ! enabled)
        secondaryReleaseRequested.store (true, std::memory_order_release);

    sc55debug::log ("2X mode %s", enabled ? "enabled" : "disabled");
    if (enabled)
        triggerAsyncUpdate();
}

void NukedSC55AudioProcessor::sendMidiToEmulators (const uint8_t* data, int size) noexcept
{
    if (data == nullptr || size <= 0)
        return;

    const auto status = data[0];
    const bool useTwoX = twoXEnabled.load (std::memory_order_acquire);

    if (! useTwoX)
    {
        emulators[0].sendMidi (data, size);
        return;
    }

    // A running-status data byte is not a complete message at this boundary;
    // keep the defensive behavior of the single-instance path.
    if (status < 0x80)
    {
        emulators[0].sendMidi (data, size);
        return;
    }

    // In 2X mode, notes are split between the two complete emulators while
    // channel state and other performance data must remain identical. System
    // messages have no MIDI channel, so they are broadcast as well.
    const auto messageType = static_cast<uint8_t> (status & 0xf0);
    if (status >= 0xf0 || (messageType != 0x80 && messageType != 0x90))
    {
        emulators[0].sendMidi (data, size);
        emulators[1].sendMidi (data, size);
        return;
    }

    const auto instance = static_cast<size_t> ((status & 0x0f) & 1u);
    emulators[instance].sendMidi (data, size);
}

void NukedSC55AudioProcessor::handleAsyncUpdate()
{
#if JUCE_STANDALONE_APPLICATION
    if (auto* holder = juce::StandalonePluginHolder::getInstance(); holder != nullptr)
    {
        for (const auto& device : juce::MidiInput::getAvailableDevices())
        {
            if (! holder->deviceManager.isMidiInputDeviceEnabled (device.identifier))
            {
                holder->deviceManager.setMidiInputDeviceEnabled (device.identifier, true);
                sc55debug::log ("enabled MIDI input \"%s\"", device.name.toRawUTF8());
            }
        }
    }
#endif

    logStandaloneAudioDeviceState (audioReady.load (std::memory_order_acquire)
                                       ? "async-ready"
                                       : "async-not-ready");

    const auto sampleRate = currentSampleRate.load (std::memory_order_acquire);
    if (audioReady.load (std::memory_order_acquire))
        return;

    const auto candidateDirectory = selectedRomDirectory.isDirectory()
        ? selectedRomDirectory
        : findRomDirectory();

    sc55debug::log ("async ROM check candidate=\"%s\" valid=%d sampleRate=%.2f requested=%d",
                    candidateDirectory.getFullPathName().toRawUTF8(),
                    candidateDirectory.isDirectory() && containsRomSet (candidateDirectory),
                    sampleRate, romSelectionRequested.load (std::memory_order_acquire) ? 1 : 0);

    if (candidateDirectory.isDirectory() && containsRomSet (candidateDirectory)
        && sampleRate > 0.0)
    {
        if (initialiseRomDirectory (candidateDirectory))
        {
            romSelectionRequested.store (false, std::memory_order_release);
            return;
        }
    }

    if (romSelectionRequested.load (std::memory_order_acquire) && romChooser == nullptr)
        launchRomChooser();
}

bool NukedSC55AudioProcessor::initialiseRomDirectory (const juce::File& directory)
{
    if (! directory.isDirectory() || ! containsRomSet (directory))
    {
        uiError = "The selected folder does not contain a usable SC-55 ROM set";
        sc55debug::log ("ROM directory rejected: \"%s\"", directory.getFullPathName().toRawUTF8());
        return false;
    }

    selectedRomDirectory = directory;
    const auto sampleRate = currentSampleRate.load (std::memory_order_acquire);
    if (sampleRate <= 0.0)
    {
        uiError.clear();
        rememberRomDirectory (directory);
        return true;
    }

    const juce::ScopedLock callbackLock (getCallbackLock());
    audioReady.store (false, std::memory_order_release);
    if (! emulators[0].initialise (directory.getFullPathName().toStdString(), sampleRate))
    {
        uiError = juce::String (emulators[0].getError());
        sc55debug::log ("ROM directory initialisation failed: %s", emulators[0].getError().c_str());
        return false;
    }

    // Construct both complete backend instances before publishing audioReady.
    // Once the audio callback starts, 2X changes only affect MIDI routing and
    // output mixing; no message-thread core lifetime change can race rendering.
    if (! emulators[1].initialise (directory.getFullPathName().toStdString(), sampleRate))
    {
        sc55debug::log ("2X secondary initialisation failed: %s; continuing with one emulator",
                        emulators[1].getError().c_str());
        twoXEnabled.store (false, std::memory_order_release);
        emulators[1].release();
    }

    uiError.clear();
    rememberRomDirectory (directory);
    audioReady.store (true, std::memory_order_release);
    sc55debug::log ("ROM directory ready: \"%s\"", directory.getFullPathName().toRawUTF8());
    return true;
}

void NukedSC55AudioProcessor::launchRomChooser()
{
    sc55debug::log ("launching ROM chooser");
    const auto weakLifetime = std::weak_ptr<int> (lifetimeToken);
    romChooser = std::make_unique<juce::FileChooser> (
        "Locate one SC-55 ROM file",
        findRomChooserDirectory(),
        "*",
        true);

    const auto chooserFlags = juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles;

    romChooser->launchAsync (chooserFlags, [this, weakLifetime] (const juce::FileChooser& chooser)
    {
        if (weakLifetime.expired())
            return;

        const auto selectedFile = chooser.getResult();
        juce::MessageManager::callAsync ([this, weakLifetime]
        {
            if (! weakLifetime.expired())
                romChooser.reset();
        });

        if (! selectedFile.existsAsFile())
            return;

        const auto directory = selectedFile.getParentDirectory();
        if (initialiseRomDirectory (directory))
        {
            romSelectionRequested.store (false, std::memory_order_release);
            triggerAsyncUpdate();
            return;
        }

        juce::String message =
            "The selected folder does not contain a usable SC-55 ROM set.\n\n"
            "Select any one of the ROM files; the other files must be in the same folder.\n\n"
            "SC-55 v1.x: sc55_rom1.bin, sc55_rom2.bin, sc55_waverom1.bin, "
            "sc55_waverom2.bin, sc55_waverom3.bin\n"
            "SC-55mkII: rom1.bin, rom2.bin, waverom1.bin, waverom2.bin, rom_sm.bin";

        if (! emulators[0].getError().empty())
            message += "\n\n" + juce::String (emulators[0].getError());

        const auto options = juce::MessageBoxOptions::makeOptionsOk (
            juce::AlertWindow::WarningIcon,
            "SC-55 ROM files not found",
            message);
        juce::AlertWindow::showAsync (options, [this, weakLifetime] (int)
        {
            if (! weakLifetime.expired())
                requestRomSelection();
        });
    });
}

//==============================================================================
bool NukedSC55AudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* NukedSC55AudioProcessor::createEditor()
{
    return new NukedSC55AudioProcessorEditor (*this);
}

//==============================================================================
void NukedSC55AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = parameters.copyState();
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void NukedSC55AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr)
    {
        if (xml->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xml));
    }
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NukedSC55AudioProcessor();
}
