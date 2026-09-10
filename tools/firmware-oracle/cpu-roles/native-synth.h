#pragma once
#include "sc55_synth.h"
#include <chrono>
#include "NativeSynthStateExchange.h"
#include <thread>

inline void VerifyNativeSynth(Emulator& reference,const RomsetInfo& roms,bool nativeVoices=false,bool allPrograms=false,unsigned notes=1,bool independent=false)
{
    sc55::SynthState meter;
    meter.voiceLevels[0]={2000,3000,1,true};
    meter.voiceLevels[1]={4000,5000,1,true};
    meter.voiceLevels[2]={50000,50000,2,true};
    meter.voiceLevels[3]={65535,65535,1,false};
    meter.calculateDisplayLevels();
    if(meter.parts[1].envelopeLevel!=9000 || meter.parts[2].envelopeLevel!=65535)
        throw std::runtime_error("UI meter lost peak/saturation/inactive-voice semantics");
    meter.voiceLevels={};meter.calculateDisplayLevels();
    if(meter.parts[1].envelopeLevel || meter.parts[2].envelopeLevel)
        throw std::runtime_error("UI meter retained an old voice level");
    NativeSynthStateExchange exchange;
    std::atomic<bool> finished{false};
    std::thread producer([&] {
        for(uint64_t sequence=1;sequence<=100000;++sequence) {
            sc55::SynthState state; state.renderedFrames=sequence;
            for(auto& part:state.parts) part.program=uint8_t(sequence%128);
            exchange.publish(state);
        }
        finished.store(true,std::memory_order_release);
    });
    bool coherent=true;
    uint64_t previous=0;
    do {
        const auto state=exchange.read();
        coherent=coherent && state.renderedFrames>=previous;
        previous=state.renderedFrames;
        for(const auto& part:state.parts) coherent=coherent && part.program==state.renderedFrames%128;
    } while(!finished.load(std::memory_order_acquire));
    producer.join();
    if(!coherent || exchange.read().renderedFrames!=100000)
        throw std::runtime_error("Native state handoff produced a torn/stale snapshot");
    std::puts("Native state: concurrent producer/consumer snapshots coherent");
    const auto& data=roms.rom_data;
    const auto bytes=sc55::ImportSoundData(data[size_t(RomLocation::ROM1)],data[size_t(RomLocation::ROM2)]);
    auto makeMode=[&](bool useNativeVoices) { return std::make_unique<sc55::NativeSynth>(bytes,
        data[size_t(RomLocation::ROM1)],data[size_t(RomLocation::ROM2)],
        data[size_t(RomLocation::WAVEROM1)],data[size_t(RomLocation::WAVEROM2)],
        data[size_t(RomLocation::WAVEROM3)],useNativeVoices ? sc55::NativeSynth::VoiceRendering::nativeVoices
            : sc55::NativeSynth::VoiceRendering::referenceChip); };
    auto make=[&] {
        if(!independent) return makeMode(nativeVoices);
        return std::make_unique<sc55::NativeSynth>(bytes,
            data[size_t(RomLocation::ROM1)],data[size_t(RomLocation::ROM2)],
            data[size_t(RomLocation::WAVEROM1)],data[size_t(RomLocation::WAVEROM2)],
            data[size_t(RomLocation::WAVEROM3)],sc55::NativeSynth::VoiceRendering::independentSignal);
    };
    auto whole=make(),split=make();
    auto chip=makeMode(false);
    std::array<AudioFrame<int32_t>,257> chipFrames{};
    long double differencePower=0,referencePower=0,fastPower=0;
    std::array<AudioFrame<int32_t>,257> a{},b{};
    uint64_t nonzero=0;
    uint64_t checksum=14695981039346656037ull;
    for(unsigned block=0;block<128;++block) {
        if(block==64 || block==96) {
            const uint8_t midi[]{uint8_t(block==64?0x90:0x80),60,100};
            whole->push(midi); split->push(midi);
            chip->push(midi);
        }
        whole->render(a);
        if(block==80) {
            const auto raw=whole->state();
            for(const auto& part:raw.parts) if(part.envelopeLevel)
                throw std::runtime_error("Audio snapshot performed display aggregation");
            auto display=raw;display.calculateDisplayLevels();
            if(!display.parts[1].envelopeLevel)
                throw std::runtime_error("Raw voice snapshot cannot reconstruct the sounding meter");
        }
        chip->render(chipFrames);
        split->render({});
        if(block&1) {
            for(auto& frame:b) split->render(std::span(&frame,1));
        } else {
            split->render(std::span(b).first(1));
            split->render(std::span(b).subspan(1,127));
            split->render(std::span(b).subspan(128));
        }
        if(whole->failed() || split->failed()) throw std::runtime_error("Native rendering failed");
        for(unsigned i=0;i<a.size();++i) {
            for(unsigned channel=0;channel<2;++channel) {
                const long double expected=channel ? chipFrames[i].right : chipFrames[i].left;
                const long double actual=channel ? a[i].right : a[i].left;
                differencePower+=(actual-expected)*(actual-expected);
                referencePower+=expected*expected; fastPower+=actual*actual;
            }
            if(a[i].left!=b[i].left || a[i].right!=b[i].right)
                throw std::runtime_error("Block partition changed native audio");
            nonzero+=(a[i].left!=0 || a[i].right!=0);
            for(const auto value:{uint32_t(a[i].left),uint32_t(a[i].right)})
                for(unsigned byte=0;byte<4;++byte) checksum=(checksum^uint8_t(value>>(byte*8)))*1099511628211ull;
        }
    }
    if(!nonzero) throw std::runtime_error("Native synth is silent");
    std::printf("Native audio checksum=%016llx\n",(unsigned long long)checksum);
    std::printf("Voice renderer vs chip relative RMS error=%.6f level ratio=%.6f\n",
        double(std::sqrt(differencePower/referencePower)),double(std::sqrt(fastPower/referencePower)));
    std::printf("Native synth: zero/1/127/129/257 frame partitions match, nonzero=%llu\n",(unsigned long long)nonzero);
    std::vector<uint8_t> programs{48,80,120};
    if(allPrograms) { programs.resize(128); for(unsigned i=0;i<128;++i) programs[i]=uint8_t(i); }
    double worstError=0; unsigned worstProgram=0,overOnePercent=0;
    if(nativeVoices) for(const uint8_t program:programs) {
        auto fast=makeMode(true),accurate=makeMode(false);
        long double error=0,power=0,outputPower=0;
        for(unsigned block=0;block<256;++block) {
            if(block==64) {
                const uint8_t change[]{0xc0,program}; fast->push(change); accurate->push(change);
                for(unsigned note=0;note<notes;++note) {
                    const uint8_t midi[]{0x90,uint8_t(notes==1 ? 60 : 48+note),100};
                    fast->push(midi); accurate->push(midi);
                }
            }
            if(block==192) {
                for(unsigned note=0;note<notes;++note) {
                    const uint8_t off[]{0x80,uint8_t(notes==1 ? 60 : 48+note),0};
                    fast->push(off); accurate->push(off);
                }
            }
            fast->render(a); accurate->render(b);
            if(fast->failed() || accurate->failed()) throw std::runtime_error("Voice renderer comparison failed");
            if(block<64) continue;
            for(unsigned i=0;i<a.size();++i) for(unsigned channel=0;channel<2;++channel) {
                const long double actual=channel?a[i].right:a[i].left;
                const long double expected=channel?b[i].right:b[i].left;
                error+=(actual-expected)*(actual-expected); power+=expected*expected; outputPower+=actual*actual;
            }
        }
        if(power==0 || outputPower==0) throw std::runtime_error("Voice renderer comparison is silent");
        std::printf("Voice renderer program=%u relative RMS error=%.6f level ratio=%.6f\n",program,
            double(std::sqrt(error/power)),double(std::sqrt(outputPower/power)));
        const auto relative=double(std::sqrt(error/power));
        if(relative>worstError) { worstError=relative; worstProgram=program; }
        overOnePercent+=relative>0.01;
        if(notes==1 && (program==48 || program==80) && error/power>0.0001L)
            throw std::runtime_error("Voice renderer exceeded 1% waveform regression tolerance");
    }
    if(allPrograms) std::printf("128-program renderer scan (%u notes): worst=%u error=%.6f above1percent=%u\n",
        notes,worstProgram,worstError,overOnePercent);
    auto native=make();
    auto& cpu=reference.GetMCU(); auto& pcm=reference.GetPCM();
    PCM_UseSimulation(pcm,nativeVoices); pcm.use_float_effects=false; pcm.enable_oversampling=false;
    auto end=cpu.cycles+120000000;
    while(cpu.cycles<end) reference.Step();
    using Clock=std::chrono::steady_clock;
    auto renderNative=[&] { for(unsigned n=0;n<125;++n) native->render(std::span(a).first(256)); };
    renderNative();
    for(unsigned phase=0;phase<2;++phase) {
        if(phase) {
            for(unsigned note=48;note<72;++note) {
                const uint8_t midi[]{0x90,uint8_t(note),100};
                reference.PostMIDI(midi); native->push(midi);
            }
            end=cpu.cycles+2000000; while(cpu.cycles<end) reference.Step();
            for(unsigned n=0;n<3200/256;++n) native->render(std::span(a).first(256));
        }
        const auto start=Clock::now(); renderNative(); const auto middle=Clock::now();
        end=cpu.cycles+20000000; while(cpu.cycles<end) reference.Step();
        const auto finish=Clock::now();
        if(native->failed()) throw std::runtime_error("Native benchmark failed");
        std::printf("CPU %s native=%.3fms H8=%.3fms per audio second (%s both)\n",phase?"24 notes":"idle",
            std::chrono::duration<double,std::milli>(middle-start).count(),
            std::chrono::duration<double,std::milli>(finish-middle).count(),nativeVoices?"native voices":"real PCM");
    }
}
