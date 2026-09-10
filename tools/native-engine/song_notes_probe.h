#pragma once
#include "MidiFilePlayer.h"
#include "sc55_native_player.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

// Replay all parts, including initialization. Inspect physical PCM gain after
// retirement during playback: allocator-only counters cannot see orphan voices.
inline int probeSongNotes(const char* path,const auto& rom,const auto& encoded)
{
    unsigned limit=128;
    if(const auto* value=std::getenv("SC55_PROBE_VOICES")) limit=unsigned(std::stoul(value));
    if(limit<24 || limit>128 || limit%4) throw std::runtime_error("Invalid probe voice limit");
    MidiFileData song;std::string error;
    if(!song.load(path,error,false)) throw std::runtime_error(error);
    double seconds=song.totalSeconds()+20;
    if(const auto* value=std::getenv("SC55_PROBE_SECONDS")) {
        const auto requested=std::stod(value);
        if(!std::isfinite(requested) || requested<=0) throw std::runtime_error("Invalid probe duration");
        seconds=std::min(seconds,requested);
    }
    const auto end=uint64_t(seconds*32000);
    const auto& r1=rom[size_t(RomLocation::ROM1)];
    const auto& r2=rom[size_t(RomLocation::ROM2)];
    sc55::SoundData data;
    if(!data.loadEncoded(encoded)) throw std::runtime_error("Invalid sound data");
    auto pcm=std::make_unique<pcm_t>();pcm->is_mk1=true;pcm->native_voice_count=limit;
    std::copy_n(rom[size_t(RomLocation::WAVEROM1)].begin(),0x100000,pcm->waverom1);
    std::copy_n(rom[size_t(RomLocation::WAVEROM2)].begin(),0x100000,pcm->waverom2);
    std::copy_n(rom[size_t(RomLocation::WAVEROM3)].begin(),0x100000,pcm->waverom3);
    sc55::NativeMelodicPlayer player(data,*pcm,sc55::ImportSystemDefaults(r1,r2),
        sc55::ImportRhythmPresets(r1,r2),sc55::ImportMelodicPresets(r1,r2),sc55::ImportEffectsTables(r1,r2));
    PCM_UseSimulation(*pcm,false);
    uint64_t frame=0;
    std::array<uint64_t,128> orphanSince{};
    std::array<unsigned,128> lastPart{};
    auto until=[&](uint64_t target) {
        while(frame<target) {
            const auto count=unsigned(std::min<uint64_t>(128,target-frame));
            player.renderFrames(count);frame+=count;
            if(player.failed()) throw std::runtime_error("Song control failed");
            const auto levels=player.voiceLevels();
            for(unsigned slot=0;slot<limit;++slot) {
                if(levels[slot].active) lastPart[slot]=levels[slot].part;
                const auto gain=PCM_PeekVoiceGainLevels(*pcm,slot);
                // The two gain stages are serial, not stereo channels.
                // Either zero silences the voice. Allow the final ramp tail.
                if(pcm->native_keys.contains(slot) && !levels[slot].active && gain[0] && gain[1]) {
                    if(!orphanSince[slot]) orphanSince[slot]=frame;
                    if(frame-orphanSince[slot]>32000) {
                        std::fprintf(stderr,"Orphan PCM voice: t=%.6f slot=%u GS-part=%u gains=%u,%u\n",
                            frame/32000.,slot,lastPart[slot],gain[0],gain[1]);
                        throw std::runtime_error("PCM still sounds one second after allocator retirement");
                    }
                } else orphanSince[slot]=0;
            }
        }
    };
    for(const auto& event:song.events) {
        const auto at=uint64_t(event.seconds*32000);
        if(at>=end) break;
        until(at);
        if(player.push(event.bytes)!=event.bytes.size()) throw std::runtime_error("Song MIDI queue full");
    }
    until(end);
    const auto remaining=player.partVoiceCount(14)+player.partVoiceCount(15);
    if(seconds>=song.totalSeconds()+20 && remaining)
        throw std::runtime_error("Guitar voices remain after natural song end");
    std::printf("PASS: no orphan PCM voices; limit=%u seconds=%.3f guitarVoices=%u\n",limit,seconds,remaining);
    return 0;
}
