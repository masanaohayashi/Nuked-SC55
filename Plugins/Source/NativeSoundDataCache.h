#pragma once

#include <juce_core/juce_core.h>
#include "sc55_sound_data_import.h"

namespace sc55
{
// Called only while preparing the emulator, before audioReady is published.
// Cache contents are deterministic; concurrent plug-in instances may safely
// publish identical files, each through its own temporary file.
inline bool EnsureNativeSoundDataCache (const std::vector<uint8_t>& rom1,
                                       const std::vector<uint8_t>& rom2,
                                       const juce::File& cacheRoot)
{
    if (! CanImportSoundData (rom1, rom2))
        return false; // Other ROM versions keep the existing emulated path.

    if (cacheRoot == juce::File())
        throw std::runtime_error ("Native sound-data cache directory is unavailable");

    const auto directory = cacheRoot.getChildFile ("mk1-v1.21-md15");
    if (directory.createDirectory().failed())
        throw std::runtime_error ("Could not create native sound-data cache directory");

    const auto target = directory.getChildFile ("sc55-native.sdata");
    juce::MemoryBlock cached;
    if (target.getSize() == 184480 && target.loadFileAsData (cached)
        && IsCurrentSoundData ({ static_cast<const uint8_t*> (cached.getData()), cached.getSize() }))
        return false;

    const auto bytes = ImportSoundData (rom1, rom2);
    const juce::TemporaryFile temporary (target);
    if (! temporary.getFile().replaceWithData (bytes.data(), bytes.size())
        || ! temporary.overwriteTargetFileWithTemporary())
        throw std::runtime_error ("Could not save native sound-data cache");

    return true;
}
}
