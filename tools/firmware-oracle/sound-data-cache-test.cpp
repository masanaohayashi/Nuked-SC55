#include "../../Plugins/Source/NativeSoundDataCache.h"
#include <iostream>

int main (int argc, char** argv)
{
    try
    {
        if (argc != 3)
            throw std::runtime_error ("Usage: sound-data-cache-test ROM_DIRECTORY NEW_CACHE_DIRECTORY");
        const juce::File romDirectory (argv[1]), cache (argv[2]);
        if (cache.exists())
            throw std::runtime_error ("Test cache directory must not already exist");
        const auto read = [] (const juce::File& file)
        {
            juce::MemoryBlock bytes;
            if (! file.loadFileAsData (bytes))
                throw std::runtime_error ("Cannot read input file");
            const auto* begin = static_cast<const uint8_t*> (bytes.getData());
            return std::vector<uint8_t> (begin, begin + bytes.getSize());
        };
        auto rom1 = read (romDirectory.getChildFile ("sc55_rom1.bin"));
        const auto rom2 = read (romDirectory.getChildFile ("sc55_rom2.bin"));
        const auto require = [] (bool condition, const char* message)
        {
            if (! condition) throw std::runtime_error (message);
        };
        require (sc55::EnsureNativeSoundDataCache (rom1, rom2, cache), "First import did not generate data");
        const auto file = cache.getChildFile ("mk1-v1.21-md15/sc55-native.sdata");
        const auto expected = read (file);
        require (sc55::IsCurrentSoundData (expected), "Generated data differs from verified MD15 export");
        const auto timestamp = file.getLastModificationTime();
        require (! sc55::EnsureNativeSoundDataCache (rom1, rom2, cache), "Cache hit regenerated data");
        require (file.getLastModificationTime() == timestamp, "Cache hit rewrote the file");
        auto corrupt = expected;
        corrupt[1234] ^= 1;
        require (file.replaceWithData (corrupt.data(), corrupt.size()), "Could not corrupt test cache");
        require (sc55::EnsureNativeSoundDataCache (rom1, rom2, cache), "Corrupt cache was not regenerated");
        require (read (file) == expected, "Repair changed output");
        require (file.replaceWithData ("SC55MD14", 8), "Could not write stale test cache");
        require (sc55::EnsureNativeSoundDataCache (rom1, rom2, cache), "Stale cache was not regenerated");
        require (read (file) == expected, "Format upgrade changed output");
        rom1[0] ^= 1;
        require (! sc55::EnsureNativeSoundDataCache (rom1, rom2, cache), "Unsupported ROM was imported");
        require (read (file) == expected, "Unsupported ROM modified cache");
        rom1[0] ^= 1;
        bool rejected = false;
        try { sc55::EnsureNativeSoundDataCache (rom1, rom2, file); }
        catch (const std::exception&) { rejected = true; }
        require (rejected, "Unwritable cache location was silently accepted");
        std::cout << "PASS: first import, exact MD15 bytes, cache reuse, corruption/stale repair, ROM guard, write failure\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
