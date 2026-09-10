#pragma once
#include "sc55_synth.h"
#include "NukedSC55Emulator.h"
#include <chrono>
#include <cstdlib>
#include <cstdio>

// Offline throughput, not Logic's real-time thread meter or callback maximum.
// Compare the same sound core with the real adapter's FIFO/resampler/MIDI path.
inline void MeasureNativeAdapterLoad(const char* directory,const RomsetInfo& roms)
{
    const auto* cache=std::getenv("SC55_TEST_CACHE");
    if(!cache) throw std::runtime_error("Set SC55_TEST_CACHE to the existing native cache");
    if(const auto* simulation=std::getenv("SC55_SIM"); simulation && std::string_view(simulation)=="1")
        throw std::runtime_error("Unset SC55_SIM: compare the same default PCM renderer on both paths");
    const auto& rom=roms.rom_data;
    const auto& rom1=rom[size_t(RomLocation::ROM1)];
    const auto& rom2=rom[size_t(RomLocation::ROM2)];
    const auto encoded=sc55::ImportSoundData(rom1,rom2);
    sc55::NativeSynth core(encoded,rom1,rom2,
        rom[size_t(RomLocation::WAVEROM1)],rom[size_t(RomLocation::WAVEROM2)],rom[size_t(RomLocation::WAVEROM3)]);
    NukedSC55Emulator adapter;
    if(!adapter.initialise(directory,48000,cache) || !adapter.getDebugState().nativeEngine)
        throw std::runtime_error("Benchmark requires the normal C++ engine, not NUKED_SC55_USE_H8=1");
    std::array<AudioFrame<int32_t>,128> source{};
    std::array<float,128> left{},right{};
    const auto renderCore=[&] {
        for(unsigned done=0;done<32000;done+=128) core.render(source);
    };
    const auto renderAdapter=[&] {
        for(unsigned done=0;done<48000;done+=128) adapter.render(left.data(),right.data(),128);
    };
    const auto send=[&](std::span<const uint8_t> message) {
        if(core.push(message)!=message.size()) throw std::runtime_error("Core MIDI rejected");
        adapter.sendMidi(message.data(),int(message.size()));
    };
    renderCore();renderAdapter();
    using Clock=std::chrono::steady_clock;
    const auto elapsed=[](auto&& render) {
        const auto start=Clock::now();render();
        return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    };
    for(unsigned phase=0;phase<2;++phase) {
        if(phase) {
            const uint8_t program[]{0xc0,80};send(program); // sustained tone, not a decaying piano benchmark
            for(unsigned key=48;key<72;++key) {
                const uint8_t note[]{0x90,uint8_t(key),100};send(note);
            }
            renderCore();renderAdapter();
        }
        std::array<double,5> direct{},host{};
        for(unsigned repeat=0;repeat<direct.size();++repeat) {
            // Alternate order to avoid always giving one side the warm CPU.
            if(repeat&1) {host[repeat]=elapsed(renderAdapter);direct[repeat]=elapsed(renderCore);}
            else {direct[repeat]=elapsed(renderCore);host[repeat]=elapsed(renderAdapter);}
        }
        std::sort(direct.begin(),direct.end());std::sort(host.begin(),host.end());
        sc55::SynthState snapshot;
        if(!adapter.getNativeState(snapshot)) throw std::runtime_error("Missing adapter state");
        adapter.render(left.data(),right.data(),128);
        if(!adapter.getNativeState(snapshot) || snapshot.failed || core.failed())
            throw std::runtime_error("Load benchmark engine failed");
        unsigned directVoices=0,hostVoices=0;
        for(const auto& part:core.state().parts) directVoices+=part.voices;
        for(const auto& part:snapshot.parts) hostVoices+=part.voices;
        if(directVoices!=(phase?24u:0u) || hostVoices!=directVoices)
            throw std::runtime_error("Load benchmark did not retain the requested voice count");
        std::printf("ADAPTER_LOAD %s voices=%u core_median_ms=%.3f adapter_median_ms=%.3f core_range=%.3f..%.3f adapter_range=%.3f..%.3f per_audio_second\n",
            phase?"24_sustained":"idle",hostVoices,direct[2],host[2],direct.front(),direct.back(),host.front(),host.back());
    }
}
