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
#endif

#include <algorithm>
#include <cstdlib>

namespace
{
const char* const romDirectoryEnvironmentVariable = "NUKED_SC55_ROM_PATH";

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
#endif
{
    sc55debug::log ("processor constructed wrapper=%d acceptsMidi=%d",
                    wrapperType, acceptsMidi() ? 1 : 0);
}

NukedSC55AudioProcessor::~NukedSC55AudioProcessor()
{
    sc55debug::log ("processor destroyed");
    cancelPendingUpdate();
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
    juce::ignoreUnused (samplesPerBlock);
    currentSampleRate.store (sampleRate, std::memory_order_release);
    triggerAsyncUpdate();
}

NukedSC55AudioProcessor::UiStatus NukedSC55AudioProcessor::getUiStatus() const
{
    UiStatus status;
    status.audioReady = audioReady.load (std::memory_order_acquire);
    status.sampleRate = currentSampleRate.load (std::memory_order_acquire);
    status.romDirectory = selectedRomDirectory.getFullPathName();
    status.error = uiError;
    status.emulator = emulator.getDebugState();
    return status;
}

void NukedSC55AudioProcessor::releaseResources()
{
    sc55debug::log ("releaseResources");

    const juce::ScopedLock callbackLock (getCallbackLock());
    const bool wasReady = audioReady.load (std::memory_order_acquire);
    audioReady.store (false, std::memory_order_release);
    emulator.release();

    // Keep MIDI received while no ROM is selected, but discard bytes belonging
    // to an already-running instance when the host tears that instance down.
    if (wasReady)
        emulator.clearPendingMidi();
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
                        emulator.isReady() ? 1 : 0);
    }

    for (int channel = 0; channel < numChannels; ++channel)
        buffer.clear (channel, 0, numSamples);

    const bool ready = audioReady.load (std::memory_order_acquire);
    auto* left = numChannels > 0 ? buffer.getWritePointer (0) : nullptr;
    auto* right = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;
    int renderedSamples = 0;

    for (const auto metadata : midiMessages)
    {
        const int eventPosition = juce::jlimit (0, numSamples, metadata.samplePosition);

        if (ready && left != nullptr && eventPosition > renderedSamples)
            emulator.render (left + renderedSamples,
                             right != nullptr ? right + renderedSamples : nullptr,
                             eventPosition - renderedSamples);

        const auto message = metadata.getMessage();
        emulator.sendMidi (message.getRawData(), message.getRawDataSize());
        renderedSamples = eventPosition;
    }

    if (ready && left != nullptr && renderedSamples < numSamples)
        emulator.render (left + renderedSamples,
                         right != nullptr ? right + renderedSamples : nullptr,
                         numSamples - renderedSamples);

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

    if (audioReady.load (std::memory_order_acquire))
        return;

    const auto sampleRate = currentSampleRate.load (std::memory_order_acquire);
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
    if (! emulator.initialise (directory.getFullPathName().toStdString(), sampleRate))
    {
        uiError = juce::String (emulator.getError());
        sc55debug::log ("ROM directory initialisation failed: %s", emulator.getError().c_str());
        return false;
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

        if (! emulator.getError().empty())
            message += "\n\n" + juce::String (emulator.getError());

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
    juce::ignoreUnused (destData);
}

void NukedSC55AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::ignoreUnused (data, sizeInBytes);
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NukedSC55AudioProcessor();
}
