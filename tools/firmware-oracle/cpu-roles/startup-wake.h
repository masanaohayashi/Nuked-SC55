#pragma once
#include "sc55_synth.h"

// Full native MIDI/admission/render path. 5710..573c sleeps on the task2
// periodic event after a failed gain-readiness poll; it does not busy-poll PCM.
inline void VerifyStartupWake(Emulator& emu,const RomsetInfo& roms)
{
    const auto& raw=roms.rom_data;
    const auto& r1=raw[size_t(RomLocation::ROM1)];
    const auto& r2=raw[size_t(RomLocation::ROM2)];
    sc55::SoundData data;
    if(!data.loadEncoded(sc55::ImportSoundData(r1,r2))) throw std::runtime_error("Invalid sound data");
    auto pcm=std::make_unique<pcm_t>();pcm->is_mk1=true;
    std::copy_n(emu.GetPCM().waverom1,sizeof(pcm->waverom1),pcm->waverom1);
    std::copy_n(emu.GetPCM().waverom2,sizeof(pcm->waverom2),pcm->waverom2);
    std::copy_n(emu.GetPCM().waverom3,sizeof(pcm->waverom3),pcm->waverom3);
    sc55::NativeMelodicPlayer player(data,*pcm,sc55::ImportSystemDefaults(r1,r2),
        sc55::ImportRhythmPresets(r1,r2),sc55::ImportMelodicPresets(r1,r2),sc55::ImportEffectsTables(r1,r2));
    const auto send=[&](std::span<const uint8_t> bytes) {
        if(player.push(bytes)!=bytes.size()) throw std::runtime_error("Startup MIDI queue full");
        player.renderFrames(1600);
    };
    const uint8_t program[]{0xc0,80};send(program);
    for(uint8_t key=48;key<72;++key) {const uint8_t note[]{0x90,key,100};send(note);}
    if(player.freeVoices()!=0) throw std::runtime_error("Startup fixture did not fill all voices");
    using Status=sc55::VoiceControlRuntime::StartStatus;
    unsigned transitions=0,unrelatedUpdates=0;
    for(uint8_t key=72;key<80;++key) {
        const uint8_t note[]{0x90,key,100};player.push(note);
        unsigned waiting=0;bool sawBusy=false,finished=false;
        for(unsigned frame=0;frame<1600;++frame) {
            const auto before=player.startupAudit();
            std::array<uint16_t,24> progress{};
            for(unsigned slot=0;slot<24;++slot)
                if(const auto* voice=player.voiceControlAudit(slot))
                    progress[slot]=voice->amplitude.state().segment.progress.position;
            const auto previousTick=pcm->cycles/sc55::ControlTaskClock::kernelTickCycles;
            if(before.status==Status::waitingForReuse) {
                ++waiting;
                for(unsigned slot=0;slot<24;++slot) if(before.channels&(1u<<slot))
                    sawBusy|=pcm->ram2[slot][9]!=0 && pcm->ram2[slot][10]!=0;
            }
            player.renderFrames(1);
            const auto after=player.startupAudit();
            if(before.status==Status::waitingForReuse && after.status==Status::waitingForReuse)
                for(unsigned slot=0;slot<24;++slot) if(!(before.channels&(1u<<slot)))
                    if(const auto* voice=player.voiceControlAudit(slot))
                        unrelatedUpdates+=progress[slot]!=voice->amplitude.state().segment.progress.position;
            if(before.status==Status::waitingForReuse && after.status!=before.status && sawBusy && waiting>2) {
                if(pcm->cycles/sc55::ControlTaskClock::kernelTickCycles==previousTick)
                    throw std::runtime_error("Reuse completed between periodic wake events");
                ++transitions;finished=true;
            }
            if(player.failed()) throw std::runtime_error("Startup wake failed engine");
        }
        if(!finished || player.startupAudit().status!=Status::complete)
            throw std::runtime_error("Startup wake fixture did not finish a busy reuse");
    }
    std::printf("Native startup wake: %u busy reuses resumed on common kernel ticks\n",transitions);
    if(!unrelatedUpdates) throw std::runtime_error("One preparing voice froze every other envelope");
    std::printf("Native startup wake: %u unrelated envelope updates while reuse waits\n",unrelatedUpdates);
    // The wake belongs to synth time, not the caller's block partitions.
    const auto encoded=sc55::ImportSoundData(r1,r2);
    const auto make=[&] {return std::make_unique<sc55::NativeSynth>(encoded,r1,r2,
        raw[size_t(RomLocation::WAVEROM1)],raw[size_t(RomLocation::WAVEROM2)],raw[size_t(RomLocation::WAVEROM3)]);};
    auto whole=make(),split=make();whole->push(program);split->push(program);
    std::array<AudioFrame<int32_t>,257> a{},b{};
    for(uint8_t key=48;key<80;++key) {
        const uint8_t note[]{0x90,key,100};whole->push(note);split->push(note);
        for(unsigned remaining=1600;remaining;) {
            const auto count=std::min(remaining,257u);
            whole->render(std::span(a).first(count));
            split->render({});
            for(unsigned offset=0;offset<count;) {
                const auto size=std::min(count-offset,offset==0 ? 1u : 127u);
                split->render(std::span(b).subspan(offset,size));offset+=size;
            }
            for(unsigned i=0;i<count;++i)
                if(a[i].left!=b[i].left || a[i].right!=b[i].right)
                    throw std::runtime_error("Startup wake depends on block partition");
            if(whole->failed() || split->failed()) throw std::runtime_error("Startup partition engine failed");
            remaining-=count;
        }
    }
    std::puts("Native startup wake: full-voice stealing audio identical across zero/1/127/257-frame blocks");
}
