#include "MidiFilePlayer.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

int main()
{
    const auto directory = std::filesystem::temp_directory_path()
        / ("sc55-sequence-path-test-" + std::to_string (std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories (directory);
    // One note followed by end-of-track; identical bytes for every filename.
    const unsigned char midi[] = {
        'M','T','h','d',0,0,0,6,0,0,0,1,0,96,
        'M','T','r','k',0,0,0,8,0,0x90,60,100,0,0xff,0x2f,0
    };
    bool passed = true;
    for (const auto* name : { u8"ascii.mid", u8"日本語の曲.mid", u8"音楽フォルダ/ascii.mid", u8"音楽フォルダ/演奏 🎵.mid" })
    {
        const auto path = directory / std::filesystem::path (name);
        std::filesystem::create_directories (path.parent_path());
        {
            std::ofstream output (path, std::ios::binary);
            output.write (reinterpret_cast<const char*> (midi), sizeof (midi));
            if (! output)
                return 1;
        }
        const auto utf8 = path.u8string();
        MidiFileData data;
        std::string error;
        const bool loaded = data.load (std::string (utf8.begin(), utf8.end()), error);
        const bool ok = loaded && data.events.size() == 1 && data.events[0].bytes == std::vector<uint8_t> { 0x90, 60, 100 };
        std::cout << (ok ? "PASS: " : "FAIL: ") << reinterpret_cast<const char*> (name) << " " << error << '\n';
        passed &= ok;
        std::filesystem::remove (path);
        const bool missingRejected = ! data.load (std::string (utf8.begin(), utf8.end()), error)
            && error == "Cannot open: " + std::string (utf8.begin(), utf8.end())
            && data.events.empty();
        std::cout << (missingRejected ? "PASS" : "FAIL") << ": missing file rejected with UTF-8 path\n";
        passed &= missingRejected;
    }
    std::filesystem::remove (directory / std::filesystem::path (u8"音楽フォルダ"));
    std::filesystem::remove (directory);
    return passed ? 0 : 1;
}
