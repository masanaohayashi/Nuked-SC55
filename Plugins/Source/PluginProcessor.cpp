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
#include <mutex>

namespace
{
#if JUCE_MAC || JUCE_IOS
constexpr const char* appGroupIdentifier = "group.tokyo.studio-r.sc55";
#endif
constexpr const char* userDataDirectoryName = "Nuked-SC55";
constexpr uint8_t gsResetMessage[] =
    { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
constexpr uint8_t gmResetMessage[] =
    { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
constexpr uint32_t midiPauseCommand = 1u << 0;
constexpr uint32_t midiStopCommand = 1u << 1;
constexpr const char* romNameStateProperty = "romName";
// Strip this property from newly-written state blobs.  It is not used for
// loading because ROMs must come from the App Group's shared library.
constexpr const char* romDirectoryStateProperty = "romDirectory";

#if ! (JUCE_MAC || JUCE_IOS)
juce::File getLegacyUserSettingsDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support")
        .getChildFile ("STUDIO-R")
        .getChildFile (userDataDirectoryName);
}
#endif

#if JUCE_MAC || JUCE_IOS
juce::File getAppGroupUserSettingsDirectory()
{
    const auto container = juce::File::getContainerForSecurityApplicationGroupIdentifier (
        appGroupIdentifier);

    if (! container.isDirectory())
    {
        DBG ("[DEBUG-SC55] App Group container unavailable id=\""
             << appGroupIdentifier << "\" path=\""
             << container.getFullPathName() << "\"");
        return {};
    }

    const auto settingsDirectory = container.getChildFile (userDataDirectoryName);
    if (! settingsDirectory.isDirectory() && settingsDirectory.createDirectory().failed())
    {
        DBG ("[DEBUG-SC55] App Group data directory could not be created path=\""
             << settingsDirectory.getFullPathName() << "\"");
        return {};
    }

    return settingsDirectory;
}
#endif

bool containsRomSet (const juce::File& directory)
{
    return directory.isDirectory()
        && NukedSC55Emulator::hasRomSet (directory.getFullPathName().toStdString());
}

juce::File getRomStorageDirectoryForProcessor();
bool isStoredRomDirectoryForProcessor (const juce::File& directory);

juce::File getRememberedRomPathFile()
{
    const auto settingsDirectory = NukedSC55AudioProcessor::getUserSettingsDirectory();
    return settingsDirectory.isDirectory()
        ? settingsDirectory.getChildFile ("rom-path.txt")
        : juce::File {};
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
    if (! isStoredRomDirectoryForProcessor (directory))
        return;

    const auto pathFile = getRememberedRomPathFile();
    if (pathFile == juce::File {})
        return;

    pathFile.getParentDirectory().createDirectory();
    pathFile.replaceWithText (directory.getFullPathName(), false, false, "\n");
}

juce::File findRomDirectory()
{
    const auto storageDirectory = getRomStorageDirectoryForProcessor();
    if (! storageDirectory.isDirectory())
        return {};

    const auto rememberedDirectory = loadRememberedRomDirectory();
    if (isStoredRomDirectoryForProcessor (rememberedDirectory)
        && containsRomSet (rememberedDirectory))
        return rememberedDirectory;

    for (const auto& directory : storageDirectory.findChildFiles (
             juce::File::findDirectories, false, "*"))
    {
        if (containsRomSet (directory))
            return directory;
    }

    return {};
}

juce::File findRomChooserDirectory()
{
    const auto storageDirectory = getRomStorageDirectoryForProcessor();
    if (storageDirectory.isDirectory())
        return storageDirectory;

    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
}

juce::File getRomStorageDirectoryForProcessor()
{
    const auto settingsDirectory = NukedSC55AudioProcessor::getUserSettingsDirectory();
    return settingsDirectory.isDirectory()
        ? settingsDirectory.getChildFile ("roms")
        : juce::File {};
}

bool isStoredRomDirectoryForProcessor (const juce::File& directory)
{
    const auto storageDirectory = getRomStorageDirectoryForProcessor();
    return storageDirectory.isDirectory()
        && directory.isDirectory()
        && directory.getParentDirectory() == storageDirectory;
}

juce::String makeRomStorageFolderName (const juce::String& sourceName)
{
    const auto folderName = juce::File::createLegalFileName (sourceName).trim();
    if (folderName.isEmpty() || folderName == "." || folderName == "..")
        return {};

    return folderName;
}

juce::File installStagedRomDirectory (const juce::File& storageDirectory,
                                      const juce::String& folderName,
                                      const juce::File& stagingDirectory)
{
    const auto destinationDirectory = storageDirectory.getChildFile (folderName);
    const auto backupDirectory = storageDirectory.getChildFile (
        juce::String (".") + folderName + ".backup");
    backupDirectory.deleteRecursively();

    const bool movedExisting = destinationDirectory.exists()
                            && destinationDirectory.moveFileTo (backupDirectory);
    if (destinationDirectory.exists()
        || ! stagingDirectory.moveFileTo (destinationDirectory))
    {
        stagingDirectory.deleteRecursively();
        if (movedExisting)
            backupDirectory.moveFileTo (destinationDirectory);
        return {};
    }

    if (movedExisting)
        backupDirectory.deleteRecursively();

    return destinationDirectory;
}

#if ! JUCE_IOS
bool copyDirectoryContents (const juce::File& sourceDirectory,
                            const juce::File& destinationDirectory,
                            int& copiedFiles)
{
    const auto sourceFiles = sourceDirectory.findChildFiles (
        juce::File::findFiles, false, "*");
    if (sourceFiles.isEmpty())
        return false;

    for (const auto& sourceFile : sourceFiles)
    {
        const auto destinationFile = destinationDirectory.getChildFile (
            sourceFile.getFileName());
        if (! sourceFile.copyFileTo (destinationFile))
        {
            DBG ("[DEBUG-SC55] failed to copy ROM file source=\""
                 + sourceFile.getFullPathName()
                 + "\" destination=\""
                 + destinationFile.getFullPathName() + "\"");
            return false;
        }

        ++copiedFiles;
    }

    return copiedFiles > 0;
}

juce::File importRomDirectoryFromFile (const juce::File& sourceDirectory)
{
    if (! sourceDirectory.isDirectory())
        return {};

    const auto folderName = makeRomStorageFolderName (sourceDirectory.getFileName());
    if (folderName.isEmpty())
        return {};

    const auto storageDirectory = getRomStorageDirectoryForProcessor();
    if (storageDirectory.createDirectory().failed() && ! storageDirectory.isDirectory())
        return {};

    const auto stagingDirectory = storageDirectory.getChildFile (
        juce::String (".") + folderName + ".importing");
    stagingDirectory.deleteRecursively();
    if (stagingDirectory.createDirectory().failed() && ! stagingDirectory.isDirectory())
        return {};

    int copiedFiles = 0;
    if (! copyDirectoryContents (sourceDirectory, stagingDirectory, copiedFiles)
        || ! containsRomSet (stagingDirectory))
    {
        DBG ("[DEBUG-SC55] ROM import rejected source=\""
             + sourceDirectory.getFullPathName()
             + "\" staging=\""
             + stagingDirectory.getFullPathName()
             + "\" copiedFiles=" + juce::String (copiedFiles));
        NukedSC55Emulator::logRomSetDiagnostics (stagingDirectory.getFullPathName().toStdString());
        stagingDirectory.deleteRecursively();
        return {};
    }

    const auto destinationDirectory = installStagedRomDirectory (
        storageDirectory, folderName, stagingDirectory);
    sc55debug::log ("ROM import source=\"%s\" destination=\"%s\" files=%d result=%d",
                    sourceDirectory.getFullPathName().toRawUTF8(),
                    destinationDirectory.getFullPathName().toRawUTF8(),
                    copiedFiles, destinationDirectory.isDirectory() ? 1 : 0);
    return destinationDirectory;
}
#endif

#if JUCE_IOS
const std::array<const char*, 34> supportedRomFileNames =
{
    "rom1.bin",
    "rom2.bin",
    "rom2_st.bin",
    "rom_sm.bin",
    "waverom1.bin",
    "waverom2.bin",
    "sc55_rom1.bin",
    "sc55_rom2.bin",
    "sc55_waverom1.bin",
    "sc55_waverom2.bin",
    "sc55_waverom3.bin",
    "cm300_rom1.bin",
    "cm300_rom2.bin",
    "cm300_waverom1.bin",
    "cm300_waverom2.bin",
    "cm300_waverom3.bin",
    "jv880_rom1.bin",
    "jv880_rom2.bin",
    "jv880_waverom1.bin",
    "jv880_waverom2.bin",
    "jv880_waverom_pcmcard.bin",
    "jv880_waverom_expansion.bin",
    "scb55_rom1.bin",
    "scb55_rom2.bin",
    "scb55_waverom1.bin",
    "scb55_waverom2.bin",
    "rlp3237_rom1.bin",
    "rlp3237_rom2.bin",
    "rlp3237_waverom1.bin",
    "sc155_rom1.bin",
    "sc155_rom2.bin",
    "sc155_waverom1.bin",
    "sc155_waverom2.bin",
    "sc155_waverom3.bin",
};

bool copyUrlToFile (const juce::URL& sourceUrl, const juce::File& destination)
{
    auto input = sourceUrl.createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress));
    if (input == nullptr)
        return false;

    const auto temporary = destination.getSiblingFile (destination.getFileName() + ".tmp");
    temporary.deleteFile();

    auto output = temporary.createOutputStream();
    if (output == nullptr)
        return false;

    std::array<char, 64 * 1024> buffer {};
    for (;;)
    {
        const auto bytesRead = input->read (buffer.data(), static_cast<int> (buffer.size()));
        if (bytesRead > 0)
        {
            if (! output->write (buffer.data(), static_cast<size_t> (bytesRead)))
            {
                temporary.deleteFile();
                return false;
            }

            continue;
        }

        if (! input->isExhausted())
        {
            temporary.deleteFile();
            return false;
        }

        break;
    }

    output->flush();
    output.reset();
    return temporary.moveFileTo (destination);
}

juce::File importRomDirectoryFromUrl (const juce::URL& sourceDirectoryUrl)
{
    const auto localFolderName = sourceDirectoryUrl.getLocalFile().getFileName();
    const auto folderNameFromUrl = localFolderName.isNotEmpty()
                                 ? localFolderName
                                 : sourceDirectoryUrl.getFileName();
    const auto folderName = makeRomStorageFolderName (folderNameFromUrl);
    if (folderName.isEmpty())
        return {};

    const auto storageDirectory = getRomStorageDirectoryForProcessor();
    if (storageDirectory.createDirectory().failed() && ! storageDirectory.isDirectory())
        return {};

    const auto stagingDirectory = storageDirectory.getChildFile (
        juce::String (".") + folderName + ".importing");

    stagingDirectory.deleteRecursively();
    if (stagingDirectory.createDirectory().failed() && ! stagingDirectory.isDirectory())
        return {};

    int copiedFiles = 0;
    const auto copyNamedFile = [&] (const char* fileName)
    {
        const auto sourceFile = sourceDirectoryUrl.getChildURL (fileName);
        if (copyUrlToFile (sourceFile, stagingDirectory.getChildFile (fileName)))
            ++copiedFiles;
    };

    for (const auto fileName : supportedRomFileNames)
        copyNamedFile (fileName);

    if (copiedFiles == 0 || ! containsRomSet (stagingDirectory))
    {
        DBG ("[DEBUG-SC55] iOS ROM import rejected source=\""
             + sourceDirectoryUrl.toString (false)
             + "\" staging=\""
             + stagingDirectory.getFullPathName()
             + "\" copiedFiles=" + juce::String (copiedFiles));
        NukedSC55Emulator::logRomSetDiagnostics (stagingDirectory.getFullPathName().toStdString());
        stagingDirectory.deleteRecursively();
        return {};
    }

    const auto destinationDirectory = installStagedRomDirectory (
        storageDirectory, folderName, stagingDirectory);
    sc55debug::log ("iOS ROM import source=\"%s\" destination=\"%s\" files=%d result=%d",
                    sourceDirectoryUrl.toString (false).toRawUTF8(),
                    destinationDirectory.getFullPathName().toRawUTF8(),
                    copiedFiles, destinationDirectory.isDirectory() ? 1 : 0);
    return destinationDirectory;
}
#endif

void logStartupDirectories()
{
#if JUCE_DEBUG
    static std::once_flag logged;
    std::call_once (logged, []
    {
        const auto sandboxDirectory = juce::File::getSpecialLocation
            (juce::File::userHomeDirectory);
        const auto documentsDirectory = juce::File::getSpecialLocation
            (juce::File::userDocumentsDirectory);
        const auto downloadsDirectory = sandboxDirectory.getChildFile ("Downloads");
        const auto settingsDirectory = NukedSC55AudioProcessor::getUserSettingsDirectory();
        const auto romStorageDirectory = settingsDirectory.getChildFile ("roms");

        DBG ("[DEBUG-SC55] startup paths sandbox=\""
             << sandboxDirectory.getFullPathName()
             << "\" documents=\""
             << documentsDirectory.getFullPathName()
             << "\" downloads=\""
             << downloadsDirectory.getFullPathName()
             << "\" settings=\""
             << settingsDirectory.getFullPathName()
             << "\" roms=\""
             << romStorageDirectory.getFullPathName()
             << "\"");
    });
#endif
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
juce::File NukedSC55AudioProcessor::getUserSettingsDirectory()
{
#if JUCE_MAC || JUCE_IOS
    return getAppGroupUserSettingsDirectory();
#else
    return getLegacyUserSettingsDirectory();
#endif
}

juce::File NukedSC55AudioProcessor::getRomStorageDirectory()
{
    return getRomStorageDirectoryForProcessor();
}

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
    logStartupDirectories();
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

juce::StringArray NukedSC55AudioProcessor::getStoredRomNames() const
{
    juce::StringArray names;
    const auto storageDirectory = getRomStorageDirectory();
    if (! storageDirectory.isDirectory())
        return names;

    for (const auto& directory : storageDirectory.findChildFiles (
             juce::File::findDirectories, false, "*"))
    {
        const auto name = directory.getFileName();
        if (name.startsWithChar ('.'))
            continue;

        if (containsRomSet (directory))
            names.add (name);
    }

    names.sortNatural();
    return names;
}

bool NukedSC55AudioProcessor::selectStoredRom (const juce::String& name)
{
    return selectStoredRomInternal (name, true);
}

bool NukedSC55AudioProcessor::selectStoredRomInternal (const juce::String& name,
                                                       bool notifyHost)
{
    if (name.isEmpty() || name == "." || name == ".."
        || name.containsAnyOf ("/\\"))
    {
        uiError = "Invalid stored ROM name";
        return false;
    }

    const auto directory = getRomStorageDirectory().getChildFile (name);
    DBG ("[DEBUG-SC55] selectStoredRom name=\"" + name
         + "\" path=\"" + directory.getFullPathName()
         + "\" isDirectory=" + juce::String (directory.isDirectory() ? 1 : 0));

    if (! directory.isDirectory() || ! containsRomSet (directory))
    {
        uiError = "The stored ROM folder is not usable";
        sc55debug::log ("stored ROM rejected: \"%s\"", directory.getFullPathName().toRawUTF8());
        NukedSC55Emulator::logRomSetDiagnostics (directory.getFullPathName().toStdString());
        return false;
    }

    if (! initialiseRomDirectory (directory))
        return false;

    if (notifyHost)
        notifyRomSelectionChanged();

    return true;
}

void NukedSC55AudioProcessor::notifyRomSelectionChanged()
{
    // ROM selection is part of the non-parameter state.  Tell the host so it
    // asks for a new state blob before the instance is closed or replaced.
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails {}
                           .withNonParameterStateChanged (true));
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
    if (wrapperType != juce::AudioProcessor::wrapperType_Standalone)
        return;

    sc55debug::log ("ROM chooser requested");
    uiError.clear();
    romSelectionRequested.store (true, std::memory_order_release);
    triggerAsyncUpdate();
}

bool NukedSC55AudioProcessor::loadRomSelection (const juce::URL& selection)
{
    juce::File directory;
    uiError.clear();

    const auto selectedPath = selection.getLocalFile();
    DBG ("[DEBUG-SC55] loadRomSelection url=\"" + selection.toString (false)
         + "\" path=\"" + selectedPath.getFullPathName()
         + "\" isDirectory=" + juce::String (selectedPath.isDirectory() ? 1 : 0)
         + " isFile=" + juce::String (selectedPath.existsAsFile() ? 1 : 0));

#if JUCE_IOS
    directory = importRomDirectoryFromUrl (selection);
#else
    // The ROM set is a directory, and App Sandbox grants access to the
    // directory the user selected.  Selecting one ROM file only grants access
    // to that file, so hashing its siblings fails under the sandbox.
    const auto selectedDirectory = selectedPath.isDirectory()
        ? selectedPath
        : selectedPath.getParentDirectory();
    directory = importRomDirectoryFromFile (selectedDirectory);
#endif

    const auto directoryIsValid = directory.isDirectory();
    DBG ("[DEBUG-SC55] loadRomSelection directory=\"" + directory.getFullPathName()
         + "\" isDirectory=" + juce::String (directoryIsValid ? 1 : 0));

    if (directoryIsValid && initialiseRomDirectory (directory))
    {
        DBG ("[DEBUG-SC55] loadRomSelection succeeded");
        notifyRomSelectionChanged();
        romSelectionRequested.store (false, std::memory_order_release);
        triggerAsyncUpdate();
        return true;
    }

    if (uiError.isEmpty())
        uiError = "Could not import the selected ROM folder";
    DBG ("[DEBUG-SC55] loadRomSelection failed error=\"" + uiError + "\"");
    return false;
}

void NukedSC55AudioProcessor::pressFrontPanelButton (NukedSC55Emulator::FrontPanelButton button)
{
    emulators[0].pressFrontPanelButton (button);
    if (twoXEnabled.load (std::memory_order_acquire))
        emulators[1].pressFrontPanelButton (button);
}

void NukedSC55AudioProcessor::requestGsReset()
{
    sc55debug::log ("GS reset requested by GS button");
    sendMidiToEmulators (gsResetMessage, static_cast<int> (sizeof (gsResetMessage)));
}

void NukedSC55AudioProcessor::requestGmReset()
{
    sc55debug::log ("GM reset requested by GM button");
    sendMidiToEmulators (gmResetMessage, static_cast<int> (sizeof (gmResetMessage)));
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
    if (! audioReady.load (std::memory_order_acquire))
    {
        const auto candidateDirectory = isStoredRomDirectoryForProcessor (selectedRomDirectory)
            ? selectedRomDirectory
            : findRomDirectory();

        sc55debug::log ("async ROM check candidate=\"%s\" valid=%d sampleRate=%.2f requested=%d",
                        candidateDirectory.getFullPathName().toRawUTF8(),
                        candidateDirectory.isDirectory() && containsRomSet (candidateDirectory),
                        sampleRate, romSelectionRequested.load (std::memory_order_acquire) ? 1 : 0);

        if (candidateDirectory.isDirectory() && containsRomSet (candidateDirectory)
            && sampleRate > 0.0
            && initialiseRomDirectory (candidateDirectory))
        {
            romSelectionRequested.store (false, std::memory_order_release);
            return;
        }
    }

    // The processor-owned chooser is only for the standalone app.  AUv3 must
    // open its chooser from the editor with an iOS parent component.
    if (wrapperType == juce::AudioProcessor::wrapperType_Standalone
        && romSelectionRequested.load (std::memory_order_acquire)
        && romChooser == nullptr)
        launchRomChooser();
}

bool NukedSC55AudioProcessor::initialiseRomDirectory (const juce::File& directory)
{
    const auto isStoredDirectory = isStoredRomDirectoryForProcessor (directory);
    const auto directoryIsValid = directory.isDirectory() && isStoredDirectory;
    const auto hasRomSet = directoryIsValid && containsRomSet (directory);
    DBG ("[DEBUG-SC55] initialiseRomDirectory path=\"" + directory.getFullPathName()
         + "\" isDirectory=" + juce::String (directoryIsValid ? 1 : 0)
         + " inAppGroup=" + juce::String (isStoredDirectory ? 1 : 0)
         + " hasRomSet=" + juce::String (hasRomSet ? 1 : 0));

    if (! directoryIsValid || ! hasRomSet)
    {
        uiError = "The selected folder does not contain a usable SC-55 ROM set";
        sc55debug::log ("ROM directory rejected: \"%s\"", directory.getFullPathName().toRawUTF8());
        NukedSC55Emulator::logRomSetDiagnostics (directory.getFullPathName().toStdString());
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
    if (wrapperType != juce::AudioProcessor::wrapperType_Standalone)
    {
        romSelectionRequested.store (false, std::memory_order_release);
        return;
    }

    sc55debug::log ("launching ROM chooser");
    const auto weakLifetime = std::weak_ptr<int> (lifetimeToken);
    romChooser = std::make_unique<juce::FileChooser> (
        "Import ROM",
        findRomChooserDirectory(),
        "*",
        true);

    const auto chooserFlags = juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectDirectories;

    romChooser->launchAsync (chooserFlags, [this, weakLifetime] (const juce::FileChooser& chooser)
    {
        if (weakLifetime.expired())
            return;

        juce::File directory;
#if JUCE_IOS
        const auto selectedDirectoryUrl = chooser.getURLResult();
        if (selectedDirectoryUrl == juce::URL {})
        {
            romSelectionRequested.store (false, std::memory_order_release);
            juce::MessageManager::callAsync ([this, weakLifetime]
            {
                if (! weakLifetime.expired())
                    romChooser.reset();
            });
            return;
        }

        directory = importRomDirectoryFromUrl (selectedDirectoryUrl);
#else
        const auto selectedDirectory = chooser.getResult();
        DBG ("[DEBUG-SC55] standalone ROM chooser returned path=\""
             + selectedDirectory.getFullPathName()
             + "\" isDirectory=" + juce::String (selectedDirectory.isDirectory() ? 1 : 0));
        if (! selectedDirectory.isDirectory())
        {
            romSelectionRequested.store (false, std::memory_order_release);
            juce::MessageManager::callAsync ([this, weakLifetime]
            {
                if (! weakLifetime.expired())
                    romChooser.reset();
            });
            return;
        }

        directory = importRomDirectoryFromFile (selectedDirectory);
#endif

        juce::MessageManager::callAsync ([this, weakLifetime]
        {
            if (! weakLifetime.expired())
                romChooser.reset();
        });

        if (directory.isDirectory() && initialiseRomDirectory (directory))
        {
            notifyRomSelectionChanged();
            romSelectionRequested.store (false, std::memory_order_release);
            triggerAsyncUpdate();
            return;
        }

        juce::String message =
            "The selected folder does not contain a usable SC-55 ROM set.\n\n"
#if JUCE_IOS
            "Select the folder containing all of the ROM files. The app will copy them into its shared roms folder.\n\n"
#else
            "Select the folder containing all of the ROM files. The app will copy them into its shared roms folder.\n\n"
#endif
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
    auto state = parameters.copyState();
    const auto selectedRomIsStored = selectedRomDirectory.isDirectory()
                                  && selectedRomDirectory.getParentDirectory()
                                         == getRomStorageDirectory();
    state.setProperty (juce::Identifier (romNameStateProperty),
                       selectedRomIsStored
                           ? selectedRomDirectory.getFileName()
                           : juce::String(),
                       nullptr);
    state.removeProperty (juce::Identifier (romDirectoryStateProperty), nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void NukedSC55AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    const auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid())
        return;

    parameters.replaceState (state);

    const auto savedRomName = state.getProperty (
        juce::Identifier (romNameStateProperty)).toString().trim();
    if (savedRomName.isNotEmpty())
    {
        DBG ("[DEBUG-SC55] restoring ROM from state name=\"" + savedRomName + "\"");
        if (selectStoredRomInternal (savedRomName, false))
            return;

        uiError = "The ROM saved in this state is unavailable. Use Import ROM";
        sc55debug::log ("state ROM unavailable: \"%s\"", savedRomName.toRawUTF8());
        return;
    }

    // Older development states may still contain an absolute romDirectory
    // property.  It is intentionally ignored: ROMs are restored only by the
    // folder name inside the App Group's shared roms directory.
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NukedSC55AudioProcessor();
}
