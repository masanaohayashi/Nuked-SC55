#include "emu.h"
#include "rom_loader.h"
#include "native-synth.h"
#include "song-allocation.h"
#include "native-adapter-load.h"
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
        if(argc==3 && std::strcmp(argv[2],"adapter-load")==0) {
            MeasureNativeAdapterLoad(argv[1],roms.romset_info);return 0;
        }
        if(argc==3 && std::strcmp(argv[2],"engine-switch")==0) {
            const auto* cache=std::getenv("SC55_TEST_CACHE");
            if(!cache) throw std::runtime_error("SC55_TEST_CACHE required");
            using Mode=NukedSC55Emulator::EngineMode;
            NukedSC55Emulator adapter;
            std::array<float,256> left{},right{};
            for(const auto mode:{Mode::native,Mode::h8,Mode::native}) {
                if(!adapter.initialise(argv[1],48000,cache,mode))
                    throw std::runtime_error(adapter.getError());
                // The original firmware must finish booting before a test note.
                // Its reset-time PCM configuration does not yet run at normal rate.
                for(int i=0;i<6000;++i) {
                    adapter.getDebugState();
                    adapter.render(left.data(),right.data(),256);
                    const auto boot=adapter.getDebugState();
                    if(i>=375 && (mode==Mode::native || boot.cycles>40000000)) break;
                    if(i==5999) throw std::runtime_error("H8 boot timeout");
                }
                const uint8_t note[]{0x90,60,100};
                adapter.sendMidi(note,3);
                double peak=0;
                for(int i=0;i<94;++i) {
                    adapter.render(left.data(),right.data(),256);
                    for(auto v:left) {
                        if(!std::isfinite(v)) throw std::runtime_error("Non-finite output");
                        peak=std::max(peak,std::abs(double(v)));
                    }
                }
                adapter.getDebugState();
                adapter.render(left.data(),right.data(),256);
                const auto state=adapter.getDebugState();
                std::printf("requested=%s active=%s ready=%d peak=%f cycles=%llu pc=%04x\n",
                    mode==Mode::native?"C++":"H8",state.nativeEngine?"C++":"H8",
                    state.ready,peak,(unsigned long long)state.cycles,state.pc);
                if(state.nativeEngine!=(mode==Mode::native) || !state.ready || peak==0
                    || (mode==Mode::h8 && state.cycles==0))
                    throw std::runtime_error("Engine selection/render failed");
                std::printf("engine=%s peak=%f cycles=%llu PASS\n",
                    state.nativeEngine?"C++":"H8",peak,(unsigned long long)state.cycles);
            }
            return 0;
        }
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
