#pragma once
#include "sc55_synth.h"
#include <cstdio>
#include <stdexcept>

// Reduced from deterministic MIDI stress, not synthesized allocator state.
// Times are milliseconds; program numbers are zero-based MIDI values.
inline int probeNativeStalls(const auto& rom,const auto& encoded)
{
    struct Event { unsigned ms,count; std::array<uint8_t,3> bytes; };
    const std::array detached{
        Event{0,2,{0xc1,21,0}}, Event{2770,3,{0xb1,127,0}},
        Event{2770,3,{0x91,66,122}}, Event{2790,3,{0x81,66,64}},
        Event{2800,3,{0xb0,126,0}}, Event{2800,3,{0x90,65,38}},
        Event{2810,3,{0x90,67,107}}, Event{2810,3,{0x91,66,74}},
        Event{2810,3,{0xb1,126,0}}};
    const std::array program{
        Event{5500,2,{0xc0,85,0}}, Event{5560,3,{0xb0,126,0}},
        Event{5560,3,{0x90,61,124}}, Event{5590,3,{0xb0,84,60}},
        Event{5590,3,{0x90,60,41}}, Event{5590,3,{0x80,60,64}},
        Event{5600,2,{0xc0,89,0}}};
    const std::array staleSource{
        Event{25640,2,{0xc0,98,0}}, Event{25670,3,{0x90,67,88}},
        Event{25680,3,{0x90,64,91}}, Event{25720,2,{0xc0,54,0}},
        Event{25760,3,{0xb0,84,67}}, Event{25760,3,{0x90,62,46}},
        Event{25800,3,{0x90,63,100}}, Event{25800,3,{0xb0,126,0}}};
    const std::array<std::span<const Event>,3> fixtures{detached,program,staleSource};
    const auto& r1=rom[size_t(RomLocation::ROM1)];
    const auto& r2=rom[size_t(RomLocation::ROM2)];
    for(unsigned capacity:{24u,64u,128u}) for(unsigned block:{1u,32u,257u})
    for(unsigned fixture=0;fixture<fixtures.size();++fixture) {
        sc55::NativeSynth synth(encoded,r1,r2,rom[size_t(RomLocation::WAVEROM1)],
            rom[size_t(RomLocation::WAVEROM2)],rom[size_t(RomLocation::WAVEROM3)],
            sc55::NativeSynth::VoiceRendering::referenceChip,capacity);
        std::array<AudioFrame<int32_t>,257> audio{};
        uint64_t frame=0;
        bool varied=false;
        std::optional<AudioFrame<int32_t>> previous;
        auto until=[&](uint64_t end) {
            while(frame<end) {
                const auto count=std::min<uint64_t>(block,end-frame);
                synth.render(std::span(audio.data(),count)); frame+=count;
                if(synth.failed()) {
                    std::fprintf(stderr,"Native stall: fixture=%u capacity=%u block=%u frame=%llu\n",
                        fixture,capacity,block,(unsigned long long)frame);
                    throw std::runtime_error("Native engine permanently stopped");
                }
                for(unsigned i=0;i<count;++i) {
                    if(previous) varied|=audio[i].left!=previous->left || audio[i].right!=previous->right;
                    previous=audio[i];
                }
            }
        };
        auto send=[&](std::span<const uint8_t> bytes) {
            if(synth.push(bytes)!=bytes.size()) throw std::runtime_error("Native engine rejected MIDI");
        };
        const auto events=fixtures[fixture];
        for(const auto& event:events) {until(event.ms*32);send(std::span(event.bytes.data(),event.count));}
        until(frame+32000);
        // A later independent note must still sound, and note-offs must drain.
        const uint8_t note[]{0xc2,0,0x92,60,100}; send(note);
        varied=false; previous.reset(); until(frame+16000);
        if(!varied) throw std::runtime_error("No changing audio after regression stream");
        // Default GS part 4 (index 3) receives MIDI channel 3.
        if(!synth.state().parts[3].voices) throw std::runtime_error("Later note was not allocated");
        for(unsigned ch=0;ch<16;++ch) {
            const uint8_t off[]{uint8_t(0xb0|ch),64,0,66,0,123,0}; send(off);
        }
        until(frame+32000*20);
        for(const auto& part:synth.state().parts)
            if(part.voices) throw std::runtime_error("Voices stuck after note release");
        std::printf("stall regression PASS fixture=%u capacity=%u block=%u\n",fixture,capacity,block);
    }
    return 0;
}
