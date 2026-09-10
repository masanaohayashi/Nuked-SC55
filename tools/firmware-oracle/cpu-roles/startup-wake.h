#pragma once
#include "sc55_synth.h"

// Exercise the product dispatcher with an already-due control event and a
// release command. Task1 (07e8..0850) precedes task8 (5af1) when both are ready.
inline void VerifyCommandControlOrder(Emulator& emu,const RomsetInfo& roms)
{
    const auto& raw=roms.rom_data;
    const auto& r1=raw[size_t(RomLocation::ROM1)];
    const auto& r2=raw[size_t(RomLocation::ROM2)];
    sc55::SoundData data;
    if(!data.loadEncoded(sc55::ImportSoundData(r1,r2))) throw std::runtime_error("Invalid sound data");
    for(unsigned backlog:{0u,64u}) {
    auto pcm=std::make_unique<pcm_t>();pcm->is_mk1=true;
    std::copy_n(emu.GetPCM().waverom1,sizeof(pcm->waverom1),pcm->waverom1);
    std::copy_n(emu.GetPCM().waverom2,sizeof(pcm->waverom2),pcm->waverom2);
    std::copy_n(emu.GetPCM().waverom3,sizeof(pcm->waverom3),pcm->waverom3);
    sc55::NativeMelodicPlayer player(data,*pcm,sc55::ImportSystemDefaults(r1,r2),
        sc55::ImportRhythmPresets(r1,r2),sc55::ImportMelodicPresets(r1,r2),sc55::ImportEffectsTables(r1,r2));
    const uint8_t note[]{0xc0,80,0x90,60,100};
    player.push(note);player.renderFrames(16000);
    unsigned slot=24;
    for(unsigned i=24;i-- >0;) if(!player.allocatorAudit().allocations[i].free()) {slot=i;break;}
    if(slot==24 || player.failed()) throw std::runtime_error("Missing command-order fixture voice");
    player.useExternalControlClockAudit();
    // Finish the pass that may already be executing at the end of warmup.
    // This fixture compares two NEW ready events, not an interrupt of old work.
    for(unsigned frame=0;frame<512 && player.controlPassPendingAudit();++frame) player.renderFrames(1);
    if(player.controlPassPendingAudit() || player.failed()) throw std::runtime_error("Old control pass did not finish");
    const uint8_t portamentoOff[]{0xb0,65,0};
    for(unsigned i=0;i<backlog;++i) player.push(portamentoOff);
    const uint8_t off[]{0x80,60,0};
    player.push(off);
    const auto beforeReadback=player.controlReadbackAudit(slot).first;
    if(!player.signalControlPassAudit(1)) throw std::runtime_error("Control event was already pending");
    // Observe inside the FIRST readback, even when the native pass completes
    // synchronously. A later release or a correcting second pass cannot pass.
    bool firstReadback=false;
    for(unsigned frame=0;frame<512 && !firstReadback && !player.failed();++frame) {
        player.renderFrames(1);
        const auto [count,stage]=player.controlReadbackAudit(slot);
        if(count!=beforeReadback && (count!=beforeReadback+1 || stage!=12))
            throw std::runtime_error("First due readback did not observe the queued release");
        firstReadback=count==beforeReadback+1;
        if(player.protectedCalculationAudit())
            throw std::runtime_error("Native product retained an instruction-time calculation wait");
    }
    const auto* voice=player.voiceControlAudit(slot);
    std::printf("Command/control simultaneous: backlog=%u queued=%zu stage=%u\n",backlog,player.queuedEvents(),
        voice ? unsigned(voice->lifecycle.stages[0]) : 255u);
    if(!firstReadback || player.failed() || player.queuedEvents() || !voice || voice->lifecycle.stages[0]!=12)
        throw std::runtime_error("Due control pass ran before the queued Note Off");
    std::puts("Native command/control priority: first due pass enters release PASS");
    }
}

// Full native MIDI/admission/render path. 5710..573c sleeps on the task2
// periodic event after a failed gain-readiness poll; it does not busy-poll PCM.
inline void VerifyStartupWake(Emulator& emu,const RomsetInfo& roms,bool boundaryReception=false)
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
    unsigned receivedBoundaries=0;
    std::array<unsigned,2> boundarySlots{24,24};
    for(uint8_t key=72;key<80;++key) {
        std::optional<uint64_t> wakeDeadline;
        bool protectedSinceWake=false;
        const uint8_t note[]{0x90,key,100};player.push(note);
        unsigned waiting=0;bool sawBusy=false,finished=false;
        for(unsigned frame=0;frame<1600;++frame) {
            const auto before=player.startupAudit();
            std::optional<unsigned> injected;
            if(boundaryReception && receivedBoundaries<2 && before.status==Status::waitingForReuse
                && !player.protectedCalculationAudit() && !pcm->irq_assert && pcm->cycles%sc55::ControlTaskClock::kernelTickCycles
                    <sc55::ControlTaskClock::kernelTickCycles-2500) {
                for(unsigned slot=0;slot<24;++slot)
                    if(!(before.channels&(1u<<slot)) && slot!=boundarySlots[0]) {
                        // Device-event fixture only: no H8 RAM, timer or
                        // sample data changes. Two different asserted voices
                        // must be acknowledged while task2 remains waiting.
                        pcm->irq_channel=uint8_t(slot);pcm->irq_assert=true;
                        injected=slot;break;
                    }
            }
            std::array<uint16_t,24> progress{};
            for(unsigned slot=0;slot<24;++slot)
                if(const auto* voice=player.voiceControlAudit(slot))
                    progress[slot]=voice->amplitude.state().segment.progress.position;
            const auto previousTick=pcm->cycles/sc55::ControlTaskClock::kernelTickCycles;
            if(const auto deadline=player.activationDeadlineAudit()) {
                const auto next=pcm->cycles+*deadline;
                if(wakeDeadline!=next) {wakeDeadline=next;protectedSinceWake=false;}
            }
            const bool protectedBefore=player.protectedCalculationAudit();
            if(before.status==Status::waitingForReuse) {
                ++waiting;
                for(unsigned slot=0;slot<24;++slot) if(before.channels&(1u<<slot))
                    sawBusy|=pcm->ram2[slot][9]!=0 && pcm->ram2[slot][10]!=0;
            }
            player.renderFrames(1);
            if(player.protectedCalculationAudit())
                throw std::runtime_error("Native product retained an instruction-time calculation wait");
            const auto after=player.startupAudit();
            protectedSinceWake|=protectedBefore && wakeDeadline && pcm->cycles>=*wakeDeadline;
            if(injected) {
                const auto* voice=player.voiceControlAudit(*injected);
                if(pcm->irq_assert || after.status!=Status::waitingForReuse
                    || !voice || !voice->lifecycle.fieldCB30)
                    throw std::runtime_error("PCM IRQ was not latched separately from deferred boundary work");
                boundarySlots[receivedBoundaries++]=*injected;
            }
            if(before.status==Status::waitingForReuse && after.status==Status::waitingForReuse)
                for(unsigned slot=0;slot<24;++slot) if(!(before.channels&(1u<<slot)))
                    if(const auto* voice=player.voiceControlAudit(slot))
                        unrelatedUpdates+=progress[slot]!=voice->amplitude.state().segment.progress.position;
            if(before.status==Status::waitingForReuse && after.status!=before.status && sawBusy && waiting>2) {
                if(!wakeDeadline || pcm->cycles<*wakeDeadline)
                    throw std::runtime_error("Reuse completed before its periodic wake deadline");
                if(pcm->cycles/sc55::ControlTaskClock::kernelTickCycles==previousTick && !protectedSinceWake)
                    throw std::runtime_error("Reuse completion was delayed without protected control work");
                ++transitions;finished=true;
            }
            if(player.failed()) throw std::runtime_error("Startup wake failed engine");
        }
        if(!finished || player.startupAudit().status!=Status::complete)
            throw std::runtime_error("Startup wake fixture did not finish a busy reuse");
        if(boundaryReception) for(auto slot:boundarySlots) if(slot<24)
            if(player.voiceControlAudit(slot)->lifecycle.fieldCB30)
                throw std::runtime_error("Latched PCM boundary was not consumed after reuse");
    }
    std::printf("Native startup wake: %u busy reuses obey common tick deadlines and protected-work delays\n",transitions);
    if(!unrelatedUpdates) throw std::runtime_error("One preparing voice froze every other envelope");
    std::printf("Native startup wake: %u unrelated envelope updates while reuse waits\n",unrelatedUpdates);
    if(boundaryReception) {
        if(receivedBoundaries!=2) throw std::runtime_error("Missing two-voice PCM reception overlap");
        std::puts("Native PCM boundary reception: two IRQs acknowledged during reuse, deferred handlers drained PASS");
    }
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
