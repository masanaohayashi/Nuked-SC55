#include "emu.h"
#include "rom_loader.h"
#include "native-synth.h"
#include "song-allocation.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>

#if defined(SC55_CONTROL_TIMING_ORACLE)
#error This target must exercise the normal product control runtime.
#endif
template<class T> concept HasInstructionTiming = requires(T& runtime) {
    runtime.advanceCalculationTime(uint64_t{});
};
static_assert(!HasInstructionTiming<sc55::VoiceControlRuntime>);

// The shared emulator sources enable observation hooks for the other tool.
// This check uses the existing song probe's PCM observations, not those hooks.
void Oracle_H8Fallback(const mcu_t&) {}
void Oracle_H8InterruptEntered(const mcu_t&,uint32_t,int32_t) {}
void Oracle_H8InterruptReturn(const mcu_t&,unsigned) {}

int main(int argc,char** argv)
{
    try {
        if(argc<3 || argc>4) return 2;
        common::LoadRomsetResult roms;
        if(common::LoadRomset(argv[1],{},common::RomLoader::Hashing,{},roms)!=common::LoadRomsetError{}
            || roms.picked_name!="mk1-v1.21") return 3;
        Emulator reference;
        if(!reference.Init({}) || !reference.LoadRoms(roms.romset,roms.romset_info)) return 4;
        reference.Reset();reference.GetMCU().native_v121_enabled=false;
        if(argc==3 && std::strcmp(argv[2],"synth")==0) {
            VerifyNativeSynth(reference,roms.romset_info);return 0;
        }
        if(argc==4 && std::strcmp(argv[2],"part16")==0)
            return CompareSongAllocation(reference,roms.romset_info,argv[3],true);
        if(argc==4 && std::strcmp(argv[2],"kick")==0)
            return CompareSongAllocation(reference,roms.romset_info,argv[3],false,true);
        return 2;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Native product check failed: %s\n",error.what());return 1;
    }
}
