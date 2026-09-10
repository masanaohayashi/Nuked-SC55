#include "sc55_synth.h"
#include "rom_loader.h"
#include <cstdio>

#if defined(SC55_CONTROL_TIMING_ORACLE) || defined(SC55_NATIVE_IO_AUDIT)
#error The native-only consumer must use normal product control code.
#endif

// Link/run check of the real public sound interface. H8 is deliberately not
// available to this executable, even as a reference or an idle fallback.
int main(int argc,char** argv)
{
    try {
        if(argc!=2) {
            std::fprintf(stderr,"Usage: sc55-native-engine-check ROM_DIRECTORY\n");
            return 2;
        }
        common::LoadRomsetResult loaded;
        if(common::LoadRomset(argv[1],"mk1-v1.21",common::RomLoader::Hashing,{},loaded)
            !=common::LoadRomsetError{}) throw std::runtime_error("Could not load SC-55 v1.21 ROMs");
        const auto& rom=loaded.romset_info.rom_data;
        const auto& rom1=rom[size_t(RomLocation::ROM1)];
        const auto& rom2=rom[size_t(RomLocation::ROM2)];
        const auto encoded=sc55::ImportSoundData(rom1,rom2);
        sc55::NativeSynth synth(encoded,rom1,rom2,
            rom[size_t(RomLocation::WAVEROM1)],rom[size_t(RomLocation::WAVEROM2)],
            rom[size_t(RomLocation::WAVEROM3)]);
        std::array<AudioFrame<int32_t>,257> frames{};
        uint64_t checksum=14695981039346656037ull,nonzero=0;
        for(unsigned block=0;block<128;++block) {
            if(block==64 || block==96) {
                const uint8_t midi[]{uint8_t(block==64 ? 0x90 : 0x80),60,100};
                if(synth.push(midi)!=sizeof(midi)) throw std::runtime_error("MIDI input rejected");
            }
            synth.render({});
            synth.render(frames);
            if(synth.failed()) throw std::runtime_error("Native sound engine failed");
            for(const auto& frame:frames) {
                nonzero+=frame.left!=0 || frame.right!=0;
                for(const auto value:{uint32_t(frame.left),uint32_t(frame.right)})
                    for(unsigned byte=0;byte<4;++byte)
                        checksum=(checksum^uint8_t(value>>(byte*8)))*1099511628211ull;
            }
        }
        // Same deterministic Note On/Off sequence and reference-PCM output
        // as the existing normal product check; not a new fidelity oracle.
        if(!nonzero || checksum!=0x3b54320560580fd3ull)
            throw std::runtime_error("Native-only output differs from the normal product fixture");
        std::printf("PASS: native MIDI/control/PCM without H8 or JUCE; checksum=%016llx nonzero=%llu\n",
            (unsigned long long)checksum,(unsigned long long)nonzero);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Native-only check failed: %s\n",error.what());
        return 1;
    }
}
