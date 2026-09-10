#pragma once
#include "allocator-state-snapshot.h"
#include "sc55_voice_control.h"
#include "sc55_voice_runtime.h"
#include "sc55_voice_engine.h"
#include "sc55_voice_prepare.h"
#include "sc55_note_setup.h"
#include "sc55_control_clock.h"
#include "sc55_channel.h"
#include "sc55_note_dispatch.h"
#include "sc55_note_start.h"
#include "melodic-allocation-test.h"
#include "sc55_sample_install.h"
#include "sc55_rhythm_admission.h"
#include "sample-install-test.h"
#include "pcm.h"
#include <fstream>
#include <iterator>
#include <source_location>
#include <filesystem>
#include <limits>

// Existing ROM loader's byte/address permutation. This path deliberately
// opens only the three named waveform files, not a complete control ROM set.
void unscramble(const uint8_t* src,uint8_t* dst,int len);

// Prepared sustain fixtures, data-only asset, real PCM register/pipeline
// implementation. No ROM loading, instruction execution, or recovered clock.
inline int verifyNativeVoiceControlPcm(const char* assetPath,const char* waveDirectory = nullptr)
{
    const auto require = [](bool ok,std::source_location at = std::source_location::current()) {
        if (!ok) throw std::runtime_error("Native voice/PCM regression at line "+std::to_string(at.line()));
    };
    std::ifstream file(assetPath,std::ios::binary);
    require(bool(file));
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
    sc55::SoundData data;
    require(data.loadEncoded(bytes) && data.pan());
    {
        // A common hardware phase is independent of the aggregated control
        // event. Consuming that event must not restart the next reuse tick.
        sc55::ControlTaskClock clock;
        const auto tick=sc55::ControlTaskClock::kernelTickCycles;
        require(clock.reset(8,tick-1));
        require(clock.untilNextKernelTick()==1 && clock.untilNextExpiration()==7*tick+1);
        require(clock.kernelTicksIn(0)==0 && clock.kernelTicksIn(1)==1
            && clock.kernelTicksIn(3*tick+1)==4);
        clock.advance(1);
        require(clock.kernelPhase()==0 && !clock.ready());
        clock.advance(7*tick+17);
        require(clock.consume()==1 && clock.kernelPhase()==17 && clock.untilNextKernelTick()==tick-17);
        require(!clock.reset(8,8*tick) && clock.kernelPhase()==17);
        auto split=clock;
        clock.advance(23*tick+119);
        split.advance(1); split.advance(23*tick); split.advance(118);
        require(clock.kernelPhase()==split.kernelPhase()
            && clock.untilNextExpiration()==split.untilNextExpiration() && clock.consume()==split.consume());
        std::puts("Native common clock: explicit phase, event consumption and split advance PASS");
    }
    verifyMelodicAllocation(data);
    verifySampleInstallation(data);
    std::array<std::vector<uint8_t>,3> waveData;
    if (waveDirectory)
        for (unsigned bank = 0; bank < 3; ++bank)
        {
            const auto path = std::filesystem::path(waveDirectory)/("sc55_waverom"+std::to_string(bank+1)+".bin");
            std::ifstream input(path,std::ios::binary);
            require(bool(input));
            const std::vector<uint8_t> raw{std::istreambuf_iterator<char>(input),{}};
            require(raw.size() == 0x100000);
            waveData[bank].resize(raw.size());
            unscramble(raw.data(),waveData[bank].data(),int(raw.size()));
        }
    auto mcu = std::make_unique<mcu_t>(); mcu->is_mk1 = true;
    auto pcm = std::make_unique<pcm_t>();
    PCM_Init(*pcm,*mcu); PCM_UseSimulation(*pcm,false);
    uint64_t frames = 0;
    mcu->callback_userdata = &frames;
    mcu->sample_callback = [](void* context,const AudioFrame<int32_t>&) { ++*static_cast<uint64_t*>(context); };
    const auto read = [&](uint8_t a) { return PCM_Read(*pcm,a); };
    const auto write = [&](uint8_t a,uint8_t v) { PCM_Write(*pcm,a,v); };
    write(0x3d,23);
    write(0,0); write(1,255); write(2,255); write(3,255);
    read(0); // Complete the device key-mask transaction.
    PCM_Update(*pcm,2500);
    require(pcm->voice_mask == 0xffffff);

    sc55::EnvelopeRunner::Setup setup{};
    setup.targets = {100,100,100,100}; setup.keyScale = setup.releaseKeyScale = 256;
    setup.plan.velocityScale1 = setup.plan.velocityScale2 = 256;
    const sc55::EnvelopeRunner::State state{{sc55::EnvelopeStage::sustain,{0,0},{0,0},100,100},25600,0xff00,65535};
    sc55::VoiceControlRuntime runtime;
    sc55::VoiceInstallationState fixtureInstallation;
    sc55::PartControllerState fixtureControllers;
    sc55::VoiceAllocator fixtureAllocator;
    auto& fixtureLinks = fixtureAllocator.pcmLinks;
    auto& voices = runtime.voices;
    for (unsigned channel = 0; channel < 24; ++channel)
    {
        voices[channel].emplace(sc55::VoiceControlState{sc55::EnvelopeRunner(setup,state)});
        voices[channel]->release.second.stage = 10; voices[channel]->release.second.target = 1000;
        voices[channel]->pitch.envelope.stage = 10; voices[channel]->pitch.envelope.output = 60000;
        runtime.first[channel].block.waveform = 3;
        runtime.first[channel].block.rateIndex = uint8_t(40+channel);
        runtime.second[channel].block.waveform = 3; runtime.second[channel].block.rateIndex = 100;
        if (channel&1) { fixtureLinks.first[channel] = uint8_t(channel-1); fixtureLinks.second[channel-1] = uint8_t(channel); }
    }
    std::array<sc55::VoiceModulation,24> modulation{};
    {
        sc55::NativeVoiceEngine engine;
        engine.runtime=runtime;
        require(engine.notes.allocator.initializeTables());
        engine.lifecycle[0].stages[0]=2;
        engine.runtime.voices[0]->lifecycle.stages[0]=12;
        engine.lifecycle[1].stages[0]=18;
        engine.lifecycle[1].fieldCAF4=4;
        engine.runtime.voices[1]->lifecycle.stages[0]=10;
        unsigned io=0;
        const auto capacity=engine.ensureCapacity(1,1,sc55::VoiceCapacityPolicy{},
            [&](uint8_t) {++io;return uint8_t(0);},[&](uint8_t,uint8_t) {++io;});
        require(capacity && *capacity && io==0);
        require(engine.lifecycle[0].stages[0]==12);
        require(engine.lifecycle[1].stages[0]==18 && engine.lifecycle[1].fieldCAF4==4);
        require(!engine.stopPartGroups(16,[](uint8_t) {return uint8_t(0);},[](uint8_t,uint8_t) {}));
        std::puts("Native admission owner: live EG and pending stop ownership preserved PASS");
    }
    std::array<uint8_t,24> sources{}; sources.fill(24);
    {
        // A high-note mapping keeps its original receive key for retirement.
        // A retry may preview again, but must not stop the group it just marked.
        sc55::NativeVoiceEngine engine;
        require(engine.notes.allocator.initializeTables());
        const auto group=engine.notes.allocator.createGroup({0,0x81,125,1,1});
        require(bool(group));
        engine.admission=sc55::NativeVoiceEngine::PendingAdmission{{sc55::NoteRequest::Action::on,0,125,100,0}};
        sc55::ChannelControls channels;
        const sc55::MidiDecoder::Event mapped{sc55::MidiDecoder::Kind::message,0x90,60,100,2};
        sc55::MelodicAllocationInputs input{0,false,0,0,0x81,125,{},0,{},uint16_t(0)};
        unsigned io=0;
        const auto load=[&](uint8_t) {++io;return uint8_t(0);};
        const auto store=[&](uint8_t,uint8_t) {++io;};
        const auto first=engine.previewMelodicAdmission(mapped,channels.channel(0),input,0x81,data,load,store);
        require(first && first->status==sc55::MelodicAllocationResult::Status::allocated
            && engine.admission->repeatedRetired && io==0);
        require(engine.notes.allocator.noteGroups[group->group].retirementFlags&4);
        const auto marked=engine.notes.allocator;
        const auto again=engine.previewMelodicAdmission(mapped,channels.channel(0),input,0x81,data,load,store);
        require(again && again->status==first->status && io==0
            && std::memcmp(&marked,&engine.notes.allocator,sizeof marked)==0);
        engine.admission=sc55::NativeVoiceEngine::PendingAdmission{{sc55::NoteRequest::Action::on,0,125,100,0}};
        input.keyRange={61,127};
        const auto rejected=engine.previewMelodicAdmission(mapped,channels.channel(0),input,0x81,data,load,store);
        require(rejected && rejected->status==sc55::MelodicAllocationResult::Status::keyRangeRejected
            && !engine.admission->repeatedRetired && io==0
            && std::memcmp(&marked,&engine.notes.allocator,sizeof marked)==0);
        std::puts("Native admission progress: original key, retry and rejected-note retirement PASS");
    }
    {
        sc55::NativeVoiceEngine engine;
        engine.preparation.reference=50;
        engine.preparation.partKeys[1]={51,52};
        using Status=sc55::VoiceControlRuntime::MelodicStartResult::Status;
        sc55::VoiceControlRuntime::MelodicStartResult result{Status::deferred,sc55::DispatchedNormalVoiceInputs{},std::nullopt};
        result.pitchHistory.prepared=1;
        result.pitchHistory.adjusted[0]=65;
        require(!engine.rememberPreparedNote(1,result,72));
        require(engine.preparation.reference==50 && engine.preparation.partKeys[1][0]==51);
        result.status=Status::preparedOnly;
        require(engine.rememberPreparedNote(1,result,72));
        require(engine.preparation.reference==72 && engine.preparation.partKeys[1][0]==65
            && engine.preparation.partKeys[1][1]==52);
        result.requests->count=1; result.requests->entries[0].slot=3;
        result.pitchHistory.adjusted[0]=66;
        require(!engine.rememberPreparedNote(1,result,73,0,38));
        require(engine.preparation.reference==72 && engine.preparation.partKeys[1][0]==65
            && engine.preparation.drumMap[3]==255);
        std::puts("Native preparation owner: deferred and incomplete work cannot commit history PASS");
    }
    {
        sc55::NativeVoiceEngine engine;
        engine.runtime=runtime;
        require(engine.notes.allocator.initializeTables());
        const auto group=engine.notes.allocator.createGroup({1,0x80,60,1,1});
        require(group.has_value());
        uint8_t flags=128;
        require(engine.installation.install(group->voices[0],{0,0,0,1,60,60,100,0,60,false},flags,engine.notes.allocator));
        auto& mono=engine.mono[1];
        mono.portamento=true;
        auto reuse=engine.monoReuseFlags(1,group->group,false,false);
        require(reuse && (*reuse&128)); // Released tail without a held key restarts.
        require(mono.held.set(60,true));
        reuse=engine.monoReuseFlags(1,group->group,false,false);
        require(reuse && !(*reuse&128));
        engine.runtime.voices[group->voices[0]]->lifecycle.fieldC8B3|=128;
        reuse=engine.monoReuseFlags(1,group->group,false,false);
        require(reuse && (*reuse&128));
        require(mono.held.set(64,true)); mono.current=64; mono.velocity=100;
        const auto release=engine.releaseMonoNote(1,64);
        require(release && release->action==sc55::MonoHeldKeys::ReleaseDecision::Action::replaceKey
            && release->replacement==60 && mono.current==64 && mono.velocity==100);
        require(!engine.releaseMonoNote(16,60));
        std::puts("Native mono owner: fresh/legato/completed EG and highest-held-key return PASS");
    }
    for (auto& mod : modulation) { mod.block.waveform = 3; mod.block.rateIndex = 100; }
    const sc55::PitchConversion conversion; const sc55::LfoWaveformTables waves;
    sc55::VoiceControlInputs input;
    input.level.velocity = input.level.master = 100;
    input.pitch.masterTune = input.pitch.partTune = 1024;
    auto uninterruptedPcm = std::make_unique<pcm_t>();
    unsigned resumedGroups = 0;
    for (unsigned tick = 0; tick < 64; ++tick)
    {
        for (unsigned channel = 0; channel < 24; ++channel)
        {
            runtime.inputs[channel] = input;
            runtime.inputs[channel].level.expression = uint8_t(64+channel+tick%32);
            runtime.inputs[channel].spatial.pan = uint8_t(channel*5);
        }
        auto uninterrupted = runtime;
        auto uninterruptedAllocator = fixtureAllocator;
        *uninterruptedPcm = *pcm;
        std::vector<uint32_t> expectedIo,actualIo;
        const auto expectedMask = uninterrupted.advance(uint16_t(tick%5),fixtureInstallation,fixtureControllers,
            uninterruptedAllocator,data,conversion,waves,
            [&](uint8_t a) { const auto v=PCM_Read(*uninterruptedPcm,a); expectedIo.push_back(0x10000u|(a<<8)|v); return v; },
            [&](uint8_t a,uint8_t v) { expectedIo.push_back((a<<8)|v); PCM_Write(*uninterruptedPcm,a,v); });
        require(runtime.beginControlPass(uint16_t(tick%5)));
        require(!runtime.beginControlPass(99)); // Must not replace the captured elapsed count.
        sc55::MidiEventQueue<2> heldMidi;
        const uint8_t message[]{0xb0,7,80};
        require(heldMidi.push(message,0).consumed==3);
        unsigned dispatched=0;
        const auto sink=[&](const auto&) { ++dispatched; return sc55::MidiDispatchResult::accepted; };
        sc55::ControlTaskClock pendingClock;
        pendingClock.advance(3u*sc55::ControlTaskClock::voicePeriodTicks*sc55::ControlTaskClock::kernelTickCycles);
        sc55::VoiceControlRuntime::ControlStep result{};
        unsigned groups=0;
        do {
            const auto delivery=dispatched==0 ? sc55::MidiDispatchResult::accepted : sc55::MidiDispatchResult::idle;
            require(runtime.serviceMidi(heldMidi,0,sink)==delivery);
            require(dispatched==1 && heldMidi.size()==0);
            require(runtime.serviceControl(pendingClock,fixtureInstallation,fixtureControllers,fixtureAllocator,
                data,conversion,waves,read,write).status==sc55::VoiceControlRuntime::ScheduledStatus::deferred);
            require(pendingClock.ready());
            const auto tracedRead=[&](uint8_t a) { const auto v=read(a); actualIo.push_back(0x10000u|(a<<8)|v); return v; };
            const auto tracedWrite=[&](uint8_t a,uint8_t v) { actualIo.push_back((a<<8)|v); write(a,v); };
            result=(tick&1)
                ? runtime.resumeControlPhase(fixtureInstallation,fixtureControllers,fixtureAllocator,data,conversion,waves,tracedRead,tracedWrite)
                : runtime.resumeControlPass(fixtureInstallation,fixtureControllers,fixtureAllocator,data,conversion,waves,tracedRead,tracedWrite);
            if(result.status==sc55::VoiceControlRuntime::ControlProgress::updatedGroup) ++groups;
            else require(result.status==sc55::VoiceControlRuntime::ControlProgress::complete
                || ((tick&1) && result.status==sc55::VoiceControlRuntime::ControlProgress::advancedPhase));
            require(groups<=12);
        } while(runtime.controlPending());
        require(groups==12 && expectedMask && result.updatedMask==*expectedMask && result.updatedMask==0xffffff);
        resumedGroups+=groups;
        require(actualIo==expectedIo && runtime.lastResults==uninterrupted.lastResults
            && runtime.lastWrites==uninterrupted.lastWrites && fixtureAllocator.activity==uninterruptedAllocator.activity);
        const auto noRead=[&](uint8_t) -> uint8_t { require(false); return 0; };
        const auto noWrite=[&](uint8_t,uint8_t) { require(false); };
        require(runtime.resumeControlPass(fixtureInstallation,fixtureControllers,fixtureAllocator,data,conversion,waves,
            noRead,noWrite).status==sc55::VoiceControlRuntime::ControlProgress::idle);
        require(pendingClock.consume()==3);
        require(runtime.serviceMidi(heldMidi,0,sink)==sc55::MidiDispatchResult::idle && dispatched==1);
        for (unsigned channel = 0; channel < 24; ++channel)
        {
            const auto& voice = *voices[channel];
            require(runtime.lastResults[channel] == sc55::VoiceControlResult::updated);
            require(runtime.lastWrites[channel] == sc55::VoicePcmUpdateResult::written);
            if (channel&1)
                require(runtime.first[channel].block.wave.phase == runtime.first[channel-1].block.wave.phase
                    && runtime.first[channel].block.rateIndex == 40+channel);
            require(pcm->ram2[channel][0] == voice.lifecycle.pcm10);
            require(pcm->ram2[channel][1] == voice.output.spatial.panWord);
            require(pcm->ram2[channel][2] == voice.output.spatial.effects);
            require(pcm->ram2[channel][3] == voice.output.tva.command);
            require(pcm->ram2[channel][4] == voice.amplitude.state().pcmWord);
            require(pcm->ram2[channel][5] == voice.second.command);
        }
        PCM_Update(*pcm,pcm->cycles+128*625); // Synthetic control cadence.
    }
    for (unsigned channel = 0; channel < 24; ++channel)
        require(voices[channel]->output.tva.ramp == 65535 && pcm->ram2[channel][9] > 0 && pcm->ram2[channel][10] > 0);
    require(frames > 0 && mcu->pc == 0 && mcu->cycles == 0);
    std::printf("Native paired runtime / real PCM: 1536 updates, 24 concurrent prepared slots in 12 pairs, %llu frames; no H8 execution (no audio fidelity claim)\n",
        static_cast<unsigned long long>(frames));
    std::printf("Resumable native control: %u groups; exact uninterrupted PCM I/O, elapsed capture and between-group MIDI delivery match\n",resumedGroups);
    {
        // Exercise the product's phases with real PCM time between readback
        // and publication. This is a synthetic duration, NOT an H8 delay model.
        auto phased=runtime;
        auto allocation=fixtureAllocator;
        allocation.pcmLinks={};
        for(unsigned slot=0;slot<23;++slot) phased.voices[slot].reset();
        auto heldPcm=std::make_unique<pcm_t>(*pcm);
        unsigned amplitudeWrites=0;
        const auto heldRead=[&](uint8_t a) { return PCM_Read(*heldPcm,a); };
        const auto heldWrite=[&](uint8_t a,uint8_t v) {
            if(a==0x18 || a==0x19) ++amplitudeWrites;
            PCM_Write(*heldPcm,a,v);
        };
        auto& voice=*phased.voices[23];
        auto moving=voice.amplitude.state(); moving.pcmWord=0x0304;
        voice.amplitude=sc55::EnvelopeRunner(voice.amplitude.setup(),moving);
        heldWrite(0x3e,23); heldWrite(0x18,3); heldWrite(0x19,4);
        require(phased.beginControlPass(1));
        using Phase=sc55::PeriodicVoiceUpdatePass::Phase;
        using Progress=sc55::VoiceControlRuntime::ControlProgress;
        const auto step=[&] {
            return phased.resumeControlPhase(fixtureInstallation,fixtureControllers,allocation,
                data,conversion,waves,heldRead,heldWrite);
        };
        require(step().status==Progress::advancedPhase && phased.controlPhase()==Phase::firstModulation);
        require(step().status==Progress::advancedPhase && phased.controlPhase()==Phase::readback);
        require(step().status==Progress::advancedPhase && phased.controlPhase()==Phase::calculate);
        const auto held=heldPcm->ram2[23][10];
        const auto stoppedWrites=amplitudeWrites;
        require(heldPcm->ram2[23][4]==0xff00 && allocation.activity[23]==voice.release.activity);
        const auto holdFor=[&](unsigned samples) {
            const auto before=heldPcm->tv_counter;
            PCM_Update(*heldPcm,heldPcm->cycles+samples*625);
            require(heldPcm->tv_counter!=before && heldPcm->ram2[23][10]==held
                && heldPcm->ram2[23][4]==0xff00 && amplitudeWrites==stoppedWrites);
        };
        holdFor(32);
        for(unsigned calculation=0;calculation<5;++calculation) {
            sc55::NativeVoiceEngine interruptedEngine;
            interruptedEngine.runtime=phased;
            interruptedEngine.notes.allocator=allocation;
            auto& interrupted=interruptedEngine.runtime;
            auto& stoppedAllocation=interruptedEngine.notes.allocator;
            auto stoppedPcm=std::make_unique<pcm_t>(*heldPcm);
            const auto stopRead=[&](uint8_t a) { return PCM_Read(*stoppedPcm,a); };
            const auto stopWrite=[&](uint8_t a,uint8_t v) { PCM_Write(*stoppedPcm,a,v); };
            for(unsigned previous=0;previous<calculation;++previous)
                require(interrupted.resumeControlPhase(fixtureInstallation,fixtureControllers,
                    stoppedAllocation,data,conversion,waves,stopRead,stopWrite).status==Progress::advancedPhase);
            require(unsigned(interrupted.controlCalculationStage())==calculation);
            auto& stopped=*interrupted.voices[23];
            stopped.stopAtSampleEnd=true;
            require(interruptedEngine.handlePcmBoundary(23,conversion,stopRead,stopWrite));
            const auto stoppedStages=stopped.lifecycle.stages;
            require(stoppedStages[0]>=14 && interruptedEngine.lifecycle[23].stages==stoppedStages
                && interrupted.first[23].firstStage==stoppedStages[0]
                && interrupted.second[23].firstStage==stoppedStages[0]);
            const auto result=interrupted.resumeControlPhase(fixtureInstallation,fixtureControllers,
                stoppedAllocation,data,conversion,waves,stopRead,stopWrite);
            require(result.status==Progress::updatedGroup
                && interrupted.lastResults[23]==sc55::VoiceControlResult::stopped
                && stopped.lifecycle.stages==stoppedStages);
        }
        for(unsigned calculation=0;calculation<5;++calculation) {
            require(unsigned(phased.controlCalculationStage())==calculation);
            require(step().status==Progress::advancedPhase);
        }
        require(phased.controlPhase()==Phase::publish);
        holdFor(32);
        require(step().status==Progress::updatedGroup && phased.controlPhase()==Phase::select);
        require(heldPcm->ram2[23][4]==voice.amplitude.state().pcmWord
            && amplitudeWrites==stoppedWrites+2);
        require(step().status==Progress::complete);
        std::puts("Native control phases: real PCM advances while readback amplitude is held through calculation until publication");
        std::puts("Native control phases: PCM sample-end stop survives all five calculation entry gates");
    }
    {
        // Detach must retain its saved lifecycle across scheduler calls.
        // Stopping the voice in that window skips the old oscillator update.
        auto phased=runtime;
        auto allocation=fixtureAllocator;allocation.pcmLinks={};
        for(unsigned slot=0;slot<23;++slot) phased.voices[slot].reset();
        phased.first[23].sharing.sharing=255;
        phased.first[23].sharing.source=22;
        require(phased.beginControlPass(3));
        const auto step=[&] {
            return phased.resumeControlPhase(fixtureInstallation,fixtureControllers,allocation,
                data,conversion,waves,read,write);
        };
        using Phase=sc55::PeriodicVoiceUpdatePass::Phase;
        using Progress=sc55::VoiceControlRuntime::ControlProgress;
        require(step().status==Progress::advancedPhase && phased.controlPhase()==Phase::firstModulation);
        require(step().status==Progress::advancedPhase && phased.controlPhase()==Phase::firstModulation);
        require(phased.first[23].sharing.sharing==0 && phased.first[23].sharing.source==24);
        const auto before=phased.first[23].block;
        phased.voices[23]->lifecycle.stages[0]=18;
        require(step().status==Progress::advancedPhase && phased.controlPhase()==Phase::readback);
        const auto& after=phased.first[23].block;
        require(after.output==before.output && after.depth==before.depth && after.delay==before.delay
            && after.attack==before.attack && after.rateIndex==before.rateIndex
            && after.wave.phase==before.wave.phase && after.wave.output==before.wave.output
            && after.wave.held==before.wave.held && after.wave.smoothed==before.wave.smoothed);
        std::puts("Native first-LFO detach: resumed lifetime check skipped stale update PASS");
    }
    for(uint16_t changedStage:{uint16_t(12),uint16_t(18)}) {
        auto phased=runtime;
        auto allocation=fixtureAllocator;allocation.pcmLinks={};
        for(unsigned slot=0;slot<23;++slot) phased.voices[slot].reset();
        phased.first[23].sharing.sharing=0;
        phased.second[23].sharing=255;phased.secondSources[23]=22;
        phased.secondSources[21]=22;
        require(phased.beginControlPass(3));
        const auto step=[&] {
            return phased.resumeControlPhase(fixtureInstallation,fixtureControllers,allocation,
                data,conversion,waves,read,write);
        };
        using Progress=sc55::VoiceControlRuntime::ControlProgress;
        for(unsigned phase=0;phase<4;++phase) require(step().status==Progress::advancedPhase);
        require(phased.controlCalculationStage()==sc55::VoiceCalculationStage::modulation);
        require(phased.second[23].sharing==0 && phased.secondSources[23]==24
            && phased.secondSources[21]==23);
        const auto before=phased.second[23].block;
        phased.voices[23]->lifecycle.stages[0]=changedStage;
        const auto result=step();
        require(result.status!=Progress::failed);
        if(changedStage==12) require(result.status==Progress::advancedPhase
            && phased.controlCalculationStage()==sc55::VoiceCalculationStage::amplitude);
        else require(phased.lastResults[23]==sc55::VoiceControlResult::stopped);
        const auto& after=phased.second[23].block;
        require(after.output==before.output && after.depth==before.depth && after.delay==before.delay
            && after.attack==before.attack && after.wave.phase==before.wave.phase
            && after.wave.output==before.wave.output && after.wave.held==before.wave.held
            && after.wave.smoothed==before.wave.smoothed);
        std::printf("Native second-LFO detach: lifecycle change to%u skipped stale update PASS\n",changedStage);
    }
    {
        // A natural EG end publishes a completed owner before the allocator
        // consumes it. The slot cannot be reused in between those phases.
        auto phased=runtime;
        sc55::VoiceAllocator allocation;
        require(allocation.initializeTables());
        const auto group=allocation.createGroup({1,0x80,60,1,1});
        require(group.has_value());
        const auto slot=group->voices[0];
        auto installation=fixtureInstallation;
        uint8_t flags=128;
        require(installation.install(slot,{0,0,0,1,60,60,100,0,60,false},flags,allocation));
        for(unsigned i=0;i<24;++i) if(i!=slot) phased.voices[i].reset();
        auto ending=phased.voices[slot]->amplitude.state();
        ending.segment.stage=sc55::EnvelopeStage::sustain;ending.segment.progress={0,0};
        ending.segment.target=0;ending.level=0;ending.pcmWord=0xff00;
        phased.voices[slot]->amplitude=sc55::EnvelopeRunner(phased.voices[slot]->amplitude.setup(),ending);
        phased.voices[slot]->lifecycle.stages[0]=10;
        phased.first[slot].sharing.sharing=0;phased.second[slot].sharing=0;
        require(phased.beginControlPass(1));
        const auto step=[&] {
            return phased.resumeControlPhase(installation,fixtureControllers,allocation,data,conversion,waves,read,write);
        };
        using Progress=sc55::VoiceControlRuntime::ControlProgress;
        for(unsigned phase=0;phase<12 && phased.lastResults[slot]!=sc55::VoiceControlResult::finished;++phase)
            require(step().status==Progress::advancedPhase);
        require(phased.lastResults[slot]==sc55::VoiceControlResult::finished
            && phased.voices[slot]->lifecycle.stages[0]==22 && allocation.freeCount==23
            && !allocation.allocations[slot].free());
        // Already-buffered commands do not go through MIDI ingress again.
        // Consume completion at their entry and reuse every slot, including
        // the just-ended one, before resuming the suspended control pass.
        sc55::NativeVoiceEngine commandOwner;
        commandOwner.runtime=phased; commandOwner.notes.allocator=allocation;
        commandOwner.installation=installation;
        require(commandOwner.serviceVoiceCompletion());
        require(commandOwner.notes.allocator.freeCount==24);
        uint8_t replacementGroup=255;
        for(unsigned key=0;key<24;++key) {
            const auto admitted=commandOwner.notes.allocator.createGroup({1,0x80,uint8_t(64+key),1,1});
            require(admitted.has_value());
            const auto destination=admitted->voices[0];
            uint8_t installFlags=128;
            require(commandOwner.installation.install(destination,
                {0,0,0,1,uint8_t(64+key),uint8_t(64+key),100,0,60,false},installFlags,commandOwner.notes.allocator));
            if(destination==slot) replacementGroup=admitted->group;
        }
        require(replacementGroup<24 && commandOwner.notes.allocator.freeCount==0);
        require(commandOwner.serviceVoiceCompletion());
        require(commandOwner.runtime.resumeControlPhase(commandOwner.installation,fixtureControllers,
            commandOwner.notes.allocator,data,conversion,waves,read,write).status==Progress::complete);
        require(commandOwner.notes.allocator.freeCount==0
            && !commandOwner.notes.allocator.allocations[slot].free()
            && commandOwner.notes.allocator.allocations[slot].noteGroup==replacementGroup);
        // MIDI must observe the returned slot, not carry an old completion
        // across the admission of a new owner.
        sc55::NativeVoiceEngine receiver;
        receiver.runtime=phased; receiver.notes.allocator=allocation;
        sc55::MidiEventQueue<4> midi;
        const std::array<uint8_t,3> note{0x90,64,100};
        require(midi.push(note,0).consumed==3);
        require(receiver.serviceMidi(midi,0,[&](const auto&) {
            require(receiver.notes.allocator.freeCount==24);
            require(receiver.notes.allocator.createGroup({1,0x80,64,1,1}).has_value());
            return sc55::MidiDispatchResult::accepted;
        })==sc55::MidiDispatchResult::accepted);
        const auto afterAdmission=receiver.notes.allocator.freeCount;
        require(afterAdmission==23);
        require(receiver.runtime.consumeVoiceCompletion(receiver.notes.allocator));
        require(receiver.notes.allocator.freeCount==afterAdmission);
        const auto returned=step();
        require(returned.status==Progress::updatedGroup && returned.changedMask==0
            && allocation.freeCount==24 && allocation.allocations[slot].free());
        require(step().status==Progress::complete && allocation.freeCount==24);
        std::puts("Native completion event: ended voice returned once, only after allocator consumption PASS");
    }
    for(bool reinstall:{false,true}) {
        // Match the observed ownership sequence: task1 stops/reinstalls while
        // task8 is between modulation and amplitude. Use the engine's actual
        // lifecycle handoff, not direct edits to its periodic voice state.
        sc55::NativeVoiceEngine engine;
        engine.runtime=runtime; engine.notes.allocator=fixtureAllocator;
        engine.notes.allocator.pcmLinks={}; engine.installation=fixtureInstallation;
        for(unsigned slot=0;slot<23;++slot) engine.runtime.voices[slot].reset();
        engine.lifecycle[23]=engine.runtime.voices[23]->lifecycle;
        auto eventPcm=std::make_unique<pcm_t>(*pcm);
        const auto eventRead=[&](uint8_t a) { return PCM_Read(*eventPcm,a); };
        const auto eventWrite=[&](uint8_t a,uint8_t v) { PCM_Write(*eventPcm,a,v); };
        using Status=sc55::VoiceControlRuntime::ScheduledStatus;
        const auto step=[&] {
            return engine.serviceControl(fixtureControllers,data,conversion,waves,eventRead,eventWrite,
                nullptr,sc55::VoiceControlRuntime::ControlSlice::phase);
        };
        engine.clock.advance(engine.clock.untilNextExpiration());
        for(unsigned phase=0;phase<4;++phase) require(step().status==Status::working);
        require(engine.runtime.controlCalculationStage()==sc55::VoiceCalculationStage::amplitude);
        engine.clock.advance(2u*sc55::ControlTaskClock::kernelTickCycles*sc55::ControlTaskClock::voicePeriodTicks);
        require(engine.requestStop(23,eventRead,eventWrite)==sc55::NativeVoiceEngine::StopRequest::queued);
        if(reinstall) {
            auto replacement=engine.installation.voices[23].input;
            replacement.part=1; replacement.originalKey=61; replacement.restarted=true;
            uint8_t flags=128;
            require(sc55::RestartAndInstallVoice(23,replacement,flags,engine.notes.allocator,
                engine.installation,engine.lifecycle[23],eventRead,eventWrite));
        }
        const auto stopped=engine.lifecycle[23].stages;
        const auto result=step();
        require(result.status==Status::working && result.elapsed==1 && result.updatedMask==(1u<<23)
            && engine.runtime.lastResults[23]==sc55::VoiceControlResult::stopped
            && engine.runtime.voices[23]->lifecycle.stages==stopped
            && engine.lifecycle[23].fieldCAF4==(reinstall ? 2 : 4));
        const auto complete=step();
        require(complete.status==Status::updated && complete.elapsed==1 && engine.clock.ready());
        require(engine.clock.consume()==2); // A continuation must not eat a new event.
        if(reinstall) require(engine.installation.voices[23].input.part==1
            && engine.installation.voices[23].input.originalKey==61);
        std::printf("Scheduled control phase: %s during calculation retained lifecycle/request and pending clock PASS\n",
            reinstall ? "stop/reinstall" : "stop");
    }
    {
        sc55::NativeVoiceEngine engine;
        engine.runtime=runtime; engine.notes.allocator=fixtureAllocator;
        engine.notes.allocator.pcmLinks={}; engine.installation=fixtureInstallation;
        for(unsigned slot=0;slot<23;++slot) engine.runtime.voices[slot].reset();
        engine.lifecycle[23]=engine.runtime.voices[23]->lifecycle;
        auto eventPcm=std::make_unique<pcm_t>(*pcm);
        const auto load=[&](uint8_t a) {return PCM_Read(*eventPcm,a);};
        const auto store=[&](uint8_t a,uint8_t v) {PCM_Write(*eventPcm,a,v);};
        engine.clock.advance(engine.clock.untilNextExpiration());
        require(engine.runtime.beginControlPass(3));
        const auto step=[&] {return engine.resumeControl(fixtureControllers,data,conversion,waves,load,store,
            sc55::VoiceControlRuntime::ControlSlice::phase);};
        using Progress=sc55::VoiceControlRuntime::ControlProgress;
        for(unsigned phase=0;phase<4;++phase) require(step().status==Progress::advancedPhase);
        require(engine.requestStop(23,load,store)==sc55::NativeVoiceEngine::StopRequest::queued);
        const auto stopped=engine.lifecycle[23].stages;
        const auto result=step();
        require(result.status==Progress::updatedGroup && result.changedMask==(1u<<23));
        require(engine.runtime.lastResults[23]==sc55::VoiceControlResult::stopped
            && engine.runtime.voices[23]->lifecycle.stages==stopped && engine.lifecycle[23].fieldCAF4==4);
        require(step().status==Progress::complete && engine.clock.consume()==1);
        std::puts("Explicit engine control: stop handoff preserved without consuming scheduled clock PASS");
    }
    {
        // A newly published owner below the scan cursor belongs to THIS pass,
        // not only the next timer event. No H8 or guessed elapsed time here.
        auto interleaved=runtime;
        auto allocation=fixtureAllocator;
        allocation.pcmLinks={};
        for(unsigned slot=0;slot<23;++slot) interleaved.voices[slot].reset();
        require(interleaved.beginControlPass(3));
        auto result=interleaved.resumeControlPass(fixtureInstallation,fixtureControllers,allocation,
            data,conversion,waves,read,write);
        require(result.status==sc55::VoiceControlRuntime::ControlProgress::updatedGroup && result.updatedMask==(1u<<23));
        sc55::MidiEventQueue<2> midi;
        const uint8_t note[]{0x90,60,100};
        require(midi.push(note,0).consumed==3);
        require(interleaved.serviceMidi(midi,0,[&](const auto& event) {
            require(event.status==0x90 && event.first==60);
            interleaved.voices[22]=runtime.voices[22];
            interleaved.inputs[22]=runtime.inputs[22];
            interleaved.inputs[22].ticks=99; // Installation cannot replace the captured pass count.
            return sc55::MidiDispatchResult::accepted;
        })==sc55::MidiDispatchResult::accepted);
        result=interleaved.resumeControlPass(fixtureInstallation,fixtureControllers,allocation,
            data,conversion,waves,read,write);
        require(result.status==sc55::VoiceControlRuntime::ControlProgress::updatedGroup
            && result.updatedMask==((1u<<23)|(1u<<22)) && result.changedMask==(1u<<22)
            && interleaved.inputs[22].ticks==3);
        require(interleaved.resumeControlPass(fixtureInstallation,fixtureControllers,allocation,
            data,conversion,waves,read,write).status==sc55::VoiceControlRuntime::ControlProgress::complete);
        std::puts("Native control interleave: a new voice owner is updated in the current pass with its captured elapsed count");
    }
    {
        auto waiting=runtime;
        auto allocation=fixtureAllocator;
        allocation.pcmLinks={};
        for(unsigned slot=0;slot<22;++slot) waiting.voices[slot].reset();
        sc55::PreparedNormalVoiceBatch prepared;
        prepared.count=1;
        prepared.voices[0]=sc55::PreparedNormalVoice{*runtime.voices[22],runtime.inputs[22],runtime.firstInputs[22],{},{}};
        prepared.voices[0]->voice.lifecycle.flagMinus3B|=128;
        std::array<sc55::VoiceStopState,24> lifecycle{};
        sc55::VoiceKeyMask keys;
        const uint8_t destination[]{22};
        require(waiting.beginControlPass(3));
        require(waiting.beginPreparedStart(destination,prepared,lifecycle,keys));
        auto keyLatch=waiting; auto latchLifecycle=lifecycle; auto latchKeys=keys;
        unsigned setupWrites=0;
        require(keyLatch.pollPreparedStart(latchLifecycle,latchKeys,[](uint8_t) { return uint8_t(0); },
            [&](uint8_t,uint8_t) { ++setupWrites; })==sc55::VoiceControlRuntime::StartStatus::waitingForKeyLatch);
        require(setupWrites>0);
        const auto noRead=[&](uint8_t) -> uint8_t { require(false); return 0; };
        const auto noWrite=[&](uint8_t,uint8_t) { require(false); };
        require(keyLatch.resumeControlPass(fixtureInstallation,fixtureControllers,allocation,data,conversion,waves,
            noRead,noWrite).status==sc55::VoiceControlRuntime::ControlProgress::deferred);
        // Reuse waits yield to unrelated voices; key-latch waits do not.
        auto result=waiting.resumeControlPass(fixtureInstallation,fixtureControllers,allocation,
            data,conversion,waves,read,write);
        require(result.status==sc55::VoiceControlRuntime::ControlProgress::updatedGroup
            && result.changedMask==(1u<<23) && keys.prepared==(1u<<22));
        result=waiting.resumeControlPass(fixtureInstallation,fixtureControllers,allocation,
            data,conversion,waves,noRead,noWrite);
        require(result.status==sc55::VoiceControlRuntime::ControlProgress::complete
            && result.changedMask==0 && result.updatedMask==(1u<<23));
        require(waiting.startupPending() && keys.prepared==(1u<<22));
        require(waiting.resumeControlPass(fixtureInstallation,fixtureControllers,allocation,
            data,conversion,waves,noRead,noWrite).status==sc55::VoiceControlRuntime::ControlProgress::idle);
        std::puts("Native reuse wait: scan skips the prepared destination without cancelling startup; key-latch phase remains protected");
    }
    {
        // Scheduled continuation publishes only this call's changed owners,
        // and does not consume ticks that arrived while the pass was held.
        auto scheduled=runtime;
        auto allocation=fixtureAllocator;allocation.pcmLinks={};
        for(unsigned slot=0;slot<22;++slot) scheduled.voices[slot].reset();
        sc55::PreparedNormalVoiceBatch prepared;prepared.count=1;
        prepared.voices[0]=sc55::PreparedNormalVoice{*runtime.voices[21],runtime.inputs[21],runtime.firstInputs[21],{},{}};
        std::array<sc55::VoiceStopState,24> lifecycle{};
        sc55::VoiceKeyMask keys;const uint8_t destination[]{21};
        require(scheduled.beginPreparedStart(destination,prepared,lifecycle,keys));
        allocation.pcmLinks.second[22]=21;
        sc55::ControlTaskClock clock;
        const auto period=clock.untilNextExpiration();clock.advance(uint64_t(period)*3);
        auto result=scheduled.serviceControl(clock,fixtureInstallation,fixtureControllers,allocation,
            data,conversion,waves,read,write);
        using Status=sc55::VoiceControlRuntime::ScheduledStatus;
        require(result.status==Status::deferred && result.elapsed==3
            && result.updatedMask==(1u<<23) && scheduled.controlPending() && !clock.ready());
        clock.advance(uint64_t(period)*2);
        // The modulation link can change between groups. Do not replay slot23
        // or lose the pass's captured count when slot22 can now proceed alone.
        allocation.pcmLinks.second[22]=255;
        result=scheduled.serviceControl(clock,fixtureInstallation,fixtureControllers,allocation,
            data,conversion,waves,read,write);
        require(result.status==Status::updated && result.elapsed==3
            && result.updatedMask==(1u<<22) && !scheduled.controlPending());
        auto pending=clock;require(pending.consume()==2);
        result=scheduled.serviceControl(clock,fixtureInstallation,fixtureControllers,allocation,
            data,conversion,waves,read,write);
        require(result.status==Status::updated && result.elapsed==2
            && result.updatedMask==((1u<<23)|(1u<<22)) && !clock.ready()
            && scheduled.startupPending() && keys.prepared==(1u<<21));
        std::puts("Scheduled control continuation: per-call ownership publication and pending tick preservation PASS");
    }
    {
        using Stop = sc55::VoiceControlRuntime::StopTaskStatus;
        for(bool restart:{false,true}) {
            sc55::NativeVoiceEngine engine;engine.runtime=runtime;
            engine.notes.allocator=fixtureAllocator;engine.notes.allocator.pcmLinks={};
            engine.installation=fixtureInstallation;
            for(unsigned slot=0;slot<24;++slot) engine.lifecycle[slot]=runtime.voices[slot]->lifecycle;
            auto isolated=std::make_unique<pcm_t>(*pcm);
            const auto load=[&](uint8_t address) {return PCM_Read(*isolated,address);};
            const auto store=[&](uint8_t address,uint8_t value) {PCM_Write(*isolated,address,value);};
            auto installed=engine.installation.voices[22].input;
            installed.part=1;installed.originalKey=61;installed.restarted=restart;
            uint8_t flags=restart ? 128 : 0;
            require(sc55::RestartAndInstallVoice(22,installed,flags,engine.notes.allocator,
                engine.installation,engine.lifecycle[22],load,store));
            auto controllers=fixtureControllers;controllers.parts[1].contributions[0][0]=100;
            const auto expected=sc55::PrepareVoiceControllers(*controllers.inputs(1,61));
            require(expected.pitchOffset!=sc55::PrepareVoiceControllers(*controllers.inputs(0,0)).pitchOffset);
            engine.clock.advance(engine.clock.untilNextExpiration());
            const auto result=engine.serviceControl(controllers,data,conversion,waves,load,store);
            require(result.status==sc55::VoiceControlRuntime::ScheduledStatus::updated
                && result.updatedMask==(restart ? (0xffffffu&~(1u<<22)) : 0xffffffu));
            require(engine.lifecycle[22].fieldCAF4==2 && engine.installation.voices[22].taskState==2
                && !engine.runtime.startupPending());
            if(!restart) require(engine.runtime.controllers[22].pitchOffset==expected.pitchOffset);
            std::printf("Queued preparation (%s): stage-based control, immediate installed identity, request retained PASS\n",
                restart ? "restart" : "continuation");
        }
        for(bool linked:{false,true})
        {
            sc55::NativeVoiceEngine engine;
            engine.runtime=runtime;engine.notes.allocator=fixtureAllocator;
            engine.notes.allocator.pcmLinks={};engine.installation=fixtureInstallation;
            if(linked) {
                engine.notes.allocator.pcmLinks.first[23]=22;
                engine.notes.allocator.pcmLinks.second[22]=23;
            }
            for(unsigned slot=0;slot<24;++slot) engine.lifecycle[slot]=runtime.voices[slot]->lifecycle;
            auto isolated=std::make_unique<pcm_t>(*pcm);
            const auto load=[&](uint8_t address) {return PCM_Read(*isolated,address);};
            const auto store=[&](uint8_t address,uint8_t value) {PCM_Write(*isolated,address,value);};
            require(engine.requestStop(22,load,store)==sc55::NativeVoiceEngine::StopRequest::queued);
            const auto stoppedStage=engine.lifecycle[22].stages[0];
            require(stoppedStage==18 || stoppedStage==20);
            engine.clock.advance(engine.clock.untilNextExpiration());
            const auto result=engine.serviceControl(fixtureControllers,data,conversion,waves,load,store);
            // With a link, scan23 selects stopped22 first. The stage18/20
            // early exit skips the remaining partner, as3363..33e7 does.
            require(result.status==sc55::VoiceControlRuntime::ScheduledStatus::updated
                && result.updatedMask==(0xffffffu&~(1u<<(linked ? 23 : 22))) && !engine.clock.ready());
            require(engine.lifecycle[22].fieldCAF4==4
                && engine.runtime.voices[22]->lifecycle.stages[0]==stoppedStage);
            // Only the stop dispatcher may finish this transition; periodic
            // exclusion must not consume its queued request or reclaim early.
            const auto dispatched=engine.serviceStopTask();
            require(dispatched.status==Stop::completed && dispatched.slot==22
                && engine.lifecycle[22].fieldCAF4==0
                && engine.lifecycle[22].stages[0]==(stoppedStage==18 ? 14 : 16));
            std::printf("Queued physical stop (%s): stage selection, unrelated updates and retained dispatch PASS\n",
                linked ? "linked" : "single");
        }
        for (uint16_t stoppedStage : {uint16_t(18),uint16_t(20)})
        {
            auto stopped = runtime; auto allocator = fixtureAllocator;
            std::array<sc55::VoiceStopState,24> lifecycle;
            for (unsigned slot = 0; slot < 24; ++slot)
            {
                lifecycle[slot] = stopped.voices[slot]->lifecycle;
                lifecycle[slot].stages.fill(stoppedStage); lifecycle[slot].fieldCAF4 = 4;
                allocator.activity[slot] = 99;
            }
            for (unsigned remaining = 24; remaining > 0; --remaining)
            {
                const auto slot = remaining-1;
                const auto result = stopped.serviceStopTask(lifecycle,allocator);
                require(result.status == Stop::completed && result.slot == slot);
                const auto expected = stoppedStage == 18 ? 14 : 16;
                require(lifecycle[slot].fieldCAF4 == 0 && allocator.activity[slot] == 0
                    && stopped.voices[slot]->lifecycle.stages[0] == expected
                    && stopped.first[slot].firstStage == expected && stopped.second[slot].firstStage == expected);
                if (slot != 0) require(lifecycle[slot-1].fieldCAF4 == 4 && allocator.activity[slot-1] == 99);
            }
            require(stopped.serviceStopTask(lifecycle,allocator).status == Stop::idle);
            lifecycle[23].fieldCAF4 = 2; lifecycle[22].fieldCAF4 = 4;
            require(stopped.serviceStopTask(lifecycle,allocator).status == Stop::needsPreparation);
            require(lifecycle[23].fieldCAF4 == 2 && lifecycle[22].fieldCAF4 == 4);
            lifecycle[23].fieldCAF4 = 4; stopped.voices[23].reset();
            require(stopped.serviceStopTask(lifecycle,allocator).status == Stop::failed && stopped.failed());
            require(lifecycle[23].fieldCAF4 == 4);
            stopped.voices[23] = runtime.voices[23];
            require(stopped.serviceStopTask(lifecycle,allocator).status == Stop::failed && lifecycle[23].fieldCAF4 == 4);
        }
    }
    {
        auto invalid = runtime;
        invalid.voices[22].reset(); // highest selected pair points at a missing source owner
        sc55::VoiceAllocator pending;
        pending.allocations[22].releaseCommand = pending.allocations[23].releaseCommand = 255;
        require(!invalid.publishNoteReleases(pending) && invalid.voices[23]->release.pending == 0);
        pending.allocations[22].releaseCommand = 0;
        require(invalid.publishNoteReleases(pending) && invalid.voices[23]->release.pending == 255);
        pending.allocations[23].releaseCommand = 0;
        require(invalid.publishNoteReleases(pending) && invalid.voices[23]->release.pending == 0);
        unsigned accesses = 0;
        const auto guardedRead = [&](uint8_t) { ++accesses; return uint8_t(0); };
        const auto guardedWrite = [&](uint8_t,uint8_t) { ++accesses; };
        require(!invalid.advance(1,fixtureInstallation,fixtureControllers,fixtureAllocator,data,conversion,waves,guardedRead,guardedWrite));
        require(invalid.failed() && accesses == 0);
        require(!invalid.publishNoteReleases(pending));
        invalid.voices[22] = runtime.voices[22];
        require(!invalid.advance(1,fixtureInstallation,fixtureControllers,fixtureAllocator,data,conversion,waves,guardedRead,guardedWrite));
        require(accesses == 0); // failure remains latched; no replay after partial recovery
        sc55::VoiceControlRuntime empty;
        sc55::VoiceAllocator emptyAllocator;
        const auto idle = empty.advance(1,fixtureInstallation,fixtureControllers,emptyAllocator,data,conversion,waves,guardedRead,guardedWrite);
        require(idle && *idle == 0 && accesses == 0);
    }

    // Replace sustain fixtures with actual patch velocity, key-scaled amplitude
    // and second-envelope preparation, sample addresses, and activation.
    // MD14 also prepares pitch. Use the recovered nominal task period, but
    // not firmware scheduling latency or contention with other tasks.
    require(data.curves() && data.levels() && data.keyLevels() && data.keys()
        && data.secondPreparation() && data.pitchEnvelope() && data.samples() && data.modulationPreparation());
    unsigned started = 0, updated = 0, released = 0, audibleLevels = 0, nativePitches = 0;
    unsigned firstOscillatorsAdvanced = 0, secondOscillatorsAdvanced = 0;
    struct OutputRange
    {
        int32_t minimum = std::numeric_limits<int32_t>::max(), maximum = std::numeric_limits<int32_t>::min();
        uint64_t frames = 0;
    } outputRange;
    unsigned waveOutputs = 0;
    unsigned mutedControlUpdates = 0;
    unsigned midiSelectedPartials = 0;
    unsigned deferredReleases = 0;
    unsigned sostenutoReleases = 0;
    unsigned controllerRefreshes = 0;
    unsigned controllerPitchDifferences = 0;
    unsigned scheduledPasses = 0;
    sc55::VoiceControlRuntime concurrent;
    sc55::PartNoteState concurrentNotes;
    require(concurrentNotes.allocator.initializeTables());
    std::array<uint8_t,24> concurrentNoteKeys{};
    std::array<sc55::PreparedVoiceBatch::Entry,24> concurrentEntries{};
    std::array<std::optional<sc55::NormalVoicePreparationInputs>,24> preparationRequests{};
    std::array<sc55::PreparedPartPitch,24> previousPartPitches{};
    std::array<unsigned,24> preparationPrograms{};
    sc55::PartNoteState noteState;
    sc55::PartialDispatchState partialState;
    sc55::VoiceInstallationState installations;
    sc55::PartControllerState partControllers;
    std::array<sc55::VoiceControllerState,24> voiceControllers{};
    std::array<sc55::VoiceStopState,24> pendingTasks{};
    auto& allocator = noteState.allocator;
    std::array<sc55::PartMidiReceive,16> routing;
    // Explicit fixture configuration, not a synthesized GS runtime default.
    for (unsigned part = 0; part < 16; ++part)
    {
        routing[part] = {uint8_t(part),0x6ea2,0x80}; // Poly/channel pressure, bend, CC and modulation reception.
        partControllers.parts[part].sensitivity = {65,64,64,64,0,0,0,64,0,0,0};
        for (auto& sensitivity : partControllers.parts[part].sourceSensitivity)
            sensitivity = {65,64,64,64,0,0,0,64,0,0,0};
        partControllers.parts[part].assignedControllers = {16,17};
    }
    require(allocator.initializeTables());
    uint32_t allocatedSlots = 0;
    std::array<uint8_t,12> scale; scale.fill(64);
    for (unsigned program = 0; program <= 128 && started < 24; ++program)
    {
        // Keep two variation-tone cases for second-LFO coverage. These are
        // explicit fixtures, not an invented capital-bank fallback.
        const bool variationFixture = program == 128;
        if (!variationFixture && started >= 22) continue;
        sc55::ChannelControls selectedControls;
        const auto midiChannel = uint8_t(program%16);
        std::optional<sc55::MelodicNoteVelocity> selected;
        if (!variationFixture)
        {
            sc55::MidiDecoder selectionDecoder;
            const std::array<uint8_t,5> bytes{uint8_t(0xc0|midiChannel),uint8_t(program),uint8_t(0x90|midiChannel),60,100};
            for (const auto byte : bytes)
                selectionDecoder.push(std::span(&byte,1),[&](const auto& event) {
                    if (!selectedControls.apply(event)
                        && sc55::AcceptNotePart(event.status&15,midiChannel,routing[midiChannel],{}))
                        selected = sc55::PrepareMelodicNoteVelocity(event,selectedControls.channel(midiChannel),0,false,0,data);
                });
            require(bool(selected));
        }
        const unsigned tone = variationFixture ? 23 : selected->tone;
        const uint8_t note = variationFixture ? 60 : selected->note;
        const uint8_t noteVelocity = variationFixture ? 100 : selected->velocity;
        const auto& patch = *data.patch(tone);
        const auto velocity = variationFixture ? sc55::PreparePatchVelocity(patch.common[6],100,
            patch.partial[0].raw,patch.partial[1].raw,0,false,*data.curves()) : selected->partials;
        for (unsigned partialIndex = 0; partialIndex < 2 && started < 24; ++partialIndex)
        {
            if (!velocity.partials[partialIndex]) continue;
            const auto& partial = patch.partial[partialIndex];
            // Reserve two cases for an enabled second oscillator. The first
            // 24 eligible v1.21 partials all have second-LFO rate zero.
            if (!variationFixture && started >= 22) continue;
            if (variationFixture && partial.raw[5] == 0) continue;
            const auto sample = sc55::PreparePartialSample(partial,*data.samples(),note,note,note,scale,0,0,note);
            if (!sample || (sample->sampleId&0x8000)) continue;
            // Each isolated PCM run reserves one actual allocator slot. Keep
            // reservations until all24 have been exercised, then return them.
            // The two opaque request fields remain explicit fixture inputs.
            const auto allocated = allocator.createGroup({midiChannel,0x80,note,1,1}); // Poly selector; A300 bit0 enables note-off.
            require(bool(allocated));
            const auto concurrentAllocation = concurrentNotes.allocator.createGroup({midiChannel,0x80,note,1,1});
            require(concurrentAllocation && concurrentAllocation->voices == allocated->voices);
            concurrentNoteKeys[allocated->voices[0]] = note;
            const auto dispatch = sc55::PlanPartialVoiceDispatch(patch,velocity.candidates.flags,
                {allocated->voices[0],255});
            require(dispatch && (*dispatch)[partialIndex].prepare);
            // Isolated-partial fixture: second partial uses the firmware's
            // first-slot fallback. Full two-partial execution is still separate.
            const uint8_t channel = (*dispatch)[partialIndex].voice;
            require(channel < 24 && (allocatedSlots&(1u<<channel)) == 0);
            allocatedSlots |= 1u<<channel;
            // Explicit previous-key fixture; boot and mono key ownership are
            // not inferred here. Prepared slot inputs now cross the same owned
            // handoff used by the native dispatch path.
            require(partialState.stage(midiChannel,partialIndex,(*dispatch)[partialIndex],
                sample->key.storedAdjustedKey.value_or(note),
                {sample->pitch.fraction,velocity.partials[partialIndex]->amplitude,
                 velocity.partials[partialIndex]->secondary,note}));
            const auto& slotInput = partialState.voices[channel];
            uint8_t preparationFlags = 160; // Explicit new/restarted normal-pitch fixture; MIDI-mode owner remains separate.
            const sc55::VoiceInstallationInput installationInput{uint16_t(tone),sample->sampleId,
                uint8_t(partialIndex),midiChannel,sample->key.storedOriginalNote.value_or(note),
                sample->key.storedAdjustedKey.value_or(note),noteVelocity,0,sample->key.lookupKey,false};
            sc55::VoiceStopState installedLifecycle;
            require(sc55::RestartAndInstallVoice(channel,installationInput,preparationFlags,allocator,
                installations,installedLifecycle,read,write));
            // The concurrent replay needs the same active allocator status;
            // createGroup alone only removes a slot from the free list.
            auto replayFlags = preparationFlags;
            require(installations.install(channel,installationInput,replayFlags,concurrentNotes.allocator));
            pendingTasks[channel] = installedLifecycle;
            const auto task = sc55::DispatchNextVoiceTask(pendingTasks,allocator.pcmLinks,allocator.activity);
            require(task && task->kind == sc55::VoiceTaskDispatch::Kind::prepare
                && task->count == 1 && task->slots[0] == channel);
            installedLifecycle = pendingTasks[channel];
            const auto& installed = installations.voices[channel];
            const auto context = sc55::PrepareVoiceContext(channel,installed,allocator.activity[channel]);
            require(context && context->slot == channel && context->part == midiChannel
                && context->partial == partialIndex && context->sample == sample->sampleId
                && data.patch(context->tone) == &patch);
            const auto samplePcm = sc55::PreparePartialSamplePcm(*sample,*data.samples(),channel,false,0);
            require(bool(samplePcm));
            // 116f..117b: envelope key is storedOriginalNote (C8FC),
            // not storedAdjustedKey (C98C) used by the sample/pitch path.
            const auto envelopeKey = installed.input.originalKey;
            const auto amplitude = sc55::PreparePartialAmplitude(patch,partialIndex,*sample,*data.samples(),
                envelopeKey,slotInput.amplitude,sc55::EnvelopeStage::attack1,64,
                *data.levels(),*data.keyLevels(),*data.keys(),*data.times());
            require(bool(amplitude));
            pcm = std::make_unique<pcm_t>(); PCM_Init(*pcm,*mcu); PCM_UseSimulation(*pcm,false);
            write(0x3d,23);
            if (waveDirectory)
            {
                std::copy(waveData[0].begin(),waveData[0].end(),pcm->waverom1);
                std::copy(waveData[1].begin(),waveData[1].end(),pcm->waverom2);
                std::copy(waveData[2].begin(),waveData[2].end(),pcm->waverom3);
            }
            write(0x3c,0); // Undithered DAC output for this integration check.
            write(0x3d,0xb7); // v1.21 boot: 24 slots and 21-bit wave-bank selection.
            outputRange = {};
            mcu->callback_userdata = &outputRange;
            mcu->sample_callback = [](void* context,const AudioFrame<int32_t>& frame) {
                auto& range = *static_cast<OutputRange*>(context);
                range.minimum = std::min({range.minimum,frame.left,frame.right});
                range.maximum = std::max({range.maximum,frame.left,frame.right});
                ++range.frames;
            };
            sc55::VoiceControlState voice{*amplitude};
            // Use the actual dispatched lifecycle, not an independent zeroed
            // post-scheduler fixture. Detailed5639 metadata expansion and paired
            // preparation still remain outside this isolated-partial test.
            voice.lifecycle = installedLifecycle;
            require(voice.lifecycle.fieldCAF4 == 0);
            voice.release.pending = installations.pendingRelease[channel];
            sc55::MidiDecoder midi;
            sc55::ChannelControls channelControls = selectedControls;
            const auto* partNoteState = noteState.part(midiChannel);
            require(partNoteState != nullptr);
            const auto cc = [&](uint8_t controller,uint8_t value) {
                const std::array<uint8_t,3> bytes{uint8_t(0xb0|midiChannel),controller,value};
                // Packet boundaries must not matter to channel state.
                for (const auto byte : bytes)
                    midi.push(std::span(&byte,1),[&](const auto& event) {
                        // Contribution side effects must not short-circuit scalar/pedal handling.
                        require(partControllers.receiveControlContributions(event,routing));
                        require(channelControls.apply(event)
                            || noteState.receivePedal(event,routing)
                            || controller == 1 || controller == 16 || controller == 17);
                    });
            };
            cc(7,100); cc(11,100); cc(91,0);
            sc55::SpatialInputs spatialInput;
            spatialInput.basePan = partial.raw[9];
            sc55::LevelInputs initialLevel; initialLevel.master = 100;
            sc55::ApplyChannelOutputControls(channelControls.channel(midiChannel),initialLevel,spatialInput);
            require(voice.output.spatial.initialize(spatialInput,*data.pan(),read,write));
            voice.prepared.pcm12 = voice.output.spatial.panWord;
            voice.prepared.pcm14 = voice.output.spatial.effects;
            voice.prepared.sample = samplePcm->address;
            voice.lifecycle.flagMinus3B = context->flags;
            sc55::PrepareVoiceAmplitude(voice.lifecycle,voice.amplitude);
            const auto& modPreparation = *data.modulationPreparation();
            modulation = {}; sources.fill(24);
            std::array<sc55::FirstModulationVoice,24> firstVoices{};
            // Each case has one live physical voice: inactive slots must not
            // become sharing candidates merely because their identity is zero.
            for (auto& v : modulation) v.firstStage = 22;
            for (auto& v : firstVoices) v.firstStage = 22;
            modulation[channel].firstStage = firstVoices[channel].firstStage = voice.lifecycle.stages[0];
            sc55::FirstModulationInputs firstInput;
            firstInput.pitchDepth = sc55::PrepareModulationDepths(partial,firstVoices[channel].block,
                modulation[channel].block,modPreparation.depths);
            // H8 +9c addresses the complete patch, including its 12-byte name.
            // Patch offsets 0e..11 therefore map to common[2..5].
            firstInput.mode = patch.common[2]; firstInput.baseRate = patch.common[3];
            firstInput.delay = patch.common[4]; firstInput.attack = patch.common[5];
            require(sc55::InitializeFirstVoiceModulation(channel,firstVoices,firstInput,modPreparation.timing,
                modPreparation.depths.pitch,*data.modulationRates(),waves,read,write) == sc55::ModulationRoute::local);
            require(sc55::PrepareVoiceSecondModulation(channel,voice.lifecycle,modulation,sources,partial,
                modPreparation.timing,*data.modulationRates(),waves,read,write) == sc55::SecondModulationPreparation::local);
            require(firstVoices[channel].sharing.baseRate == patch.common[3]
                && modulation[channel].block.rateIndex == partial.raw[5]);
            sc55::SecondEnvelopeSetup secondSetup;
            secondSetup.input.suppressPositiveControl = (partial.raw[8]&4) != 0;
            secondSetup.timing.attackControlEnabled = (partial.raw[8]&16) != 0;
            sc55::PitchModulationInputs initialPitchControls;
            initialPitchControls.masterTune = initialPitchControls.partTune = 1024;
            sc55::VoiceControlInputs preparationControls;
            preparationControls.level = initialLevel; preparationControls.spatial = spatialInput;
            preparationControls.pitch = initialPitchControls;
            // Explicit nonneutral configuration; GS defaults are not inferred.
            // All five contribution producers and per-key pressure now feed
            // native pitch preparation without any control-ROM execution.
            cc(1,32); cc(16,16); cc(17,8);
            const auto bend = [&](uint16_t value) {
                const std::array<uint8_t,3> bytes{uint8_t(0xe0|midiChannel),uint8_t(value&127),uint8_t(value>>7)};
                for (auto byte : bytes) midi.push(std::span(&byte,1),[&](const auto& event) {
                    require(partControllers.receivePitchBend(event,routing));
                });
            };
            bend(12288);
            for (auto byte : std::array<uint8_t,2>{uint8_t(0xd0|midiChannel),24})
                midi.push(std::span(&byte,1),[&](const auto& event) {
                    require(partControllers.receiveChannelPressure(event,routing));
                });
            const std::array<uint8_t,3> pressure{uint8_t(0xa0|midiChannel),installed.input.originalKey,96};
            for (const auto byte : pressure)
                midi.push(std::span(&byte,1),[&](const auto& event) { require(partControllers.receivePolyPressure(event,routing)); });
            const auto controllerInput = partControllers.inputs(context->part,installed.input.originalKey);
            require(controllerInput && controllerInput->keyValue == 96);
            const auto controllerState = sc55::PrepareVoiceControllers(*controllerInput);
            for (const auto& row : controllerInput->contributions) require(row[0] != 0);
            require(controllerState.pitchOffset != 0);
            controllerState.apply(initialLevel,secondSetup.input,initialPitchControls,
                firstVoices[channel].block,modulation[channel].block);
            sc55::ApplyVoiceModulationOutputs(firstVoices[channel].block,modulation[channel].block,
                initialLevel,secondSetup.input,initialPitchControls);
            voice.output.tva.initialize(initialLevel);
            sc55::PrepareVoiceTva(voice.lifecycle,voice.output.tva);
            const auto& prep = *data.secondPreparation();
            require(secondSetup.prepare(partial,envelopeKey,
                slotInput.secondary,voice.release.second,voice.second,
                prep.keys,prep.targets,data.pitchEnvelope()->curve,prep.timing,data.keys()->multipliers,
                *data.times(),*data.secondEnvelope()));
            sc55::VoicePostEnable post;
            sc55::PrepareVoiceSecondEnvelope(voice.lifecycle,voice.prepared,post,
                voice.release.second,secondSetup,voice.second);
            voice.pitch.envelope.stage = 10; voice.pitch.envelope.output = 60000;
            uint32_t samplePitchReference = 0;
            if (data.pitchTiming())
            {
                const sc55::NormalPitchStartInputs pitchInput{
                    installed.input.adjustedKey,envelopeKey,installed.input.velocity,slotInput.pitchFraction,
                    installed.flags,note,0,0,initialPitchControls};
                const auto preparedPitch = sc55::PrepareNormalVoicePitch(patch,partialIndex,*sample,
                    pitchInput,{},data,conversion,read,write);
                require(bool(preparedPitch));
                // Reused voices must take live runner state, not the snapshot
                // saved at the previous note's preparation. Zero elapsed makes
                // preservation of every legal stage/progress independently visible.
                for (uint16_t stage=0;stage<=22;stage+=2) {
                    auto live=preparedPitch->runner;
                    live.envelope.stage=stage;
                    live.envelope.segment.progress={12345,0};
                    live.glide.increment=7654;
                    live.glide.pitch.correction={77,321};
                    auto reusedInput=pitchInput;
                    reusedInput.flags=0x40; reusedInput.sourceKey=255;
                    reusedInput.correctionSource=77;
                    const auto reused=sc55::PrepareNormalVoicePitch(patch,partialIndex,*sample,
                        reusedInput,preparedPitch->part,data,conversion,
                        [](uint8_t) { return uint8_t(0); },[](uint8_t,uint8_t) {},&live,0);
                    require(bool(reused));
                    require(reused->runner.envelope.stage==stage
                        && reused->runner.envelope.segment.progress.position==12345);
                    require(reused->runner.glide.pitch.correction.source==77
                        && reused->runner.glide.pitch.correction.offset==321);
                    require(reused->runner.glide.increment==((7654+preparedPitch->part.values.pitch
                        -reused->part.values.pitch)&0xffffff));
                }
                samplePitchReference = preparedPitch->part.values.reference;
                voice.pitch = preparedPitch->runner;
                ++nativePitches;
            }
            sc55::PrepareVoicePitch(voice.lifecycle,voice.pitch);

            preparationRequests[channel] = sc55::NormalVoicePreparationInputs{installed,*sample,
                slotInput.amplitude,slotInput.secondary,note,slotInput.pitchFraction,false,0,preparationControls,{}};
            preparationPrograms[channel] = program;
            auto preparedFirst = firstVoices; auto preparedSecond = modulation; auto preparedSources = sources;
            const auto composed = sc55::PrepareNormalVoiceDsp(channel,*preparationRequests[channel],installedLifecycle,
                {},partControllers,preparedFirst,preparedSecond,preparedSources,data,conversion,waves,read,write);
            require(bool(composed));
            {
                auto live=composed->voice;
                auto amplitudeState=live.amplitude.state();
                amplitudeState.segment.progress={12345,7};
                live.amplitude=sc55::EnvelopeRunner(live.amplitude.setup(),amplitudeState);
                live.release.second.progress={23456,9};
                live.pitch.envelope.stage=4; live.pitch.envelope.segment.progress={34567,0};
                sc55::PrepareVoiceAmplitude(live.lifecycle,live.amplitude);
                live.lifecycle.stages[2]=4;
                auto reusedRequest=*preparationRequests[channel];
                reusedRequest.installed.flags=0x5f;
                reusedRequest.controls=composed->controls;
                reusedRequest.controls.ticks=0;
                sc55::NormalVoicePreparationEntry entry{channel,reusedRequest,live.lifecycle,composed->partPitch,&live};
                auto reuseFirst=preparedFirst; auto reuseSecond=preparedSecond; auto reuseSources=preparedSources;
                reuseFirst[channel].block.wave.phase=1234;
                reuseSecond[channel].block.wave.phase=5678;
                const auto reused=sc55::PrepareNormalVoicesDsp(std::span(&entry,1),partControllers,
                    reuseFirst,reuseSecond,reuseSources,data,conversion,waves,
                    [](uint8_t) { return uint8_t(0); },[](uint8_t,uint8_t) {});
                require(reused && reused->voices[0]);
                auto next=reused->voices[0]->voice;
                require(next.lifecycle.stages[0]==24 && next.lifecycle.savedStage==live.lifecycle.stages[0]);
                require(next.amplitude.state().segment.progress.position==12345
                    && next.amplitude.state().segment.progress.deferredTicks==7
                    && next.release.second.progress.position==23456
                    && next.release.second.progress.deferredTicks==9
                    && next.pitch.envelope.stage==4 && next.pitch.envelope.segment.progress.position==34567);
                require(reuseFirst[channel].block.wave.phase==1234 && reuseSecond[channel].block.wave.phase==5678);
                auto activated=next.lifecycle;
                sc55::ActivatePreparedVoice(activated,[](uint8_t,uint8_t) {});
                require(sc55::ContinueVoiceControlAfterStart(next,activated));
                require(next.lifecycle.stages[0]==live.lifecycle.stages[0]
                    && next.amplitude.state().segment.progress.position==12345);
                sc55::VoiceControlRuntime reuseRuntime;
                reuseRuntime.voices[channel]=live;
                reuseRuntime.inputs[channel]=composed->controls;
                reuseRuntime.inputs[channel].secondLimit=37;
                reuseRuntime.first=preparedFirst; reuseRuntime.second=preparedSecond;
                reuseRuntime.secondSources=preparedSources;
                std::array<sc55::VoiceStopState,24> reuseLifecycle{};
                reuseLifecycle[channel]=live.lifecycle;
                sc55::VoiceKeyMask reuseMask;
                entry.continuing=nullptr; // Runtime must bind its own live owner.
                const auto runtimePrepared=reuseRuntime.prepareAndBeginNormalStart(std::span(&entry,1),
                    reuseLifecycle,reuseMask,partControllers,data,conversion,waves,
                    [](uint8_t) { return uint8_t(0); },[](uint8_t,uint8_t) {});
                require(runtimePrepared && reuseRuntime.startupPending()
                    && runtimePrepared->voices[0]->controls.secondLimit==37
                    && reuseLifecycle[channel].stages[0]==24);
            }
            require(preparedFirst[channel].commonIdentity == installed.input.tone
                && preparedFirst[channel].field9b == installed.input.part
                && preparedSecond[channel].partialIdentity == installed.input.tone
                && preparedSecond[channel].field99 == installed.input.partial
                && preparedSecond[channel].field9b == installed.input.part);
            {
                auto peersFirst = preparedFirst; auto peersSecond = preparedSecond;
                const auto peer = (channel+1)%24;
                peersFirst[peer] = peersFirst[channel]; peersSecond[peer] = peersSecond[channel];
                require(sc55::SelectFirstModulationSource(channel,16,peersFirst) == peer
                    && sc55::SelectSecondModulationSource(channel,16,peersSecond) == peer);
                peersFirst[peer].field9b ^= 1; peersSecond[peer].field9b ^= 1;
                require(sc55::SelectFirstModulationSource(channel,16,peersFirst) == 24
                    && sc55::SelectSecondModulationSource(channel,16,peersSecond) == 24);
                peersFirst[peer] = peersFirst[channel]; peersSecond[peer] = peersSecond[channel];
                peersFirst[peer].commonIdentity ^= 1; peersSecond[peer].field99 ^= 1;
                require(sc55::SelectFirstModulationSource(channel,16,peersFirst) == 24
                    && sc55::SelectSecondModulationSource(channel,16,peersSecond) == 24);
            }
            if (started == 0)
            {
                auto rateControllers = partControllers;
                rateControllers.parts[midiChannel].contributions[0][3] = 32000;
                rateControllers.parts[midiChannel].contributions[0][7] = 32000;
                const auto rateState = sc55::PrepareVoiceControllers(*rateControllers.inputs(midiChannel,installed.input.originalKey));
                require(rateState.rateModifiers[0] != 0);
                auto actualFirst = firstVoices; auto actualSecond = modulation; auto actualSources = sources;
                const auto ratePrepared = sc55::PrepareNormalVoiceDsp(channel,*preparationRequests[channel],installedLifecycle,
                    {},rateControllers,actualFirst,actualSecond,actualSources,data,conversion,waves,read,write);
                require(bool(ratePrepared));
                auto expected = actualFirst[channel].block;
                expected.rateModifier = int16_t(rateState.rateModifiers[0]);
                require(sc55::InitializeFirstModulation(expected,ratePrepared->firstControls,modPreparation.timing,
                    modPreparation.depths.pitch,*data.modulationRates(),waves,read,write));
                if (actualFirst[channel].block.wave.phase != expected.wave.phase)
                    throw std::runtime_error("Startup LFO missed controller rate on its first tick");
                auto expectedSecond = actualSecond[channel].block;
                expectedSecond.rateModifier = int16_t(rateState.rateModifiers[1]);
                sc55::InitializeSecondModulation(expectedSecond,partial,modPreparation.timing,
                    *data.modulationRates(),waves,read,write);
                require(actualSecond[channel].block.wave.phase == expectedSecond.wave.phase
                    && actualSecond[channel].block.output == expectedSecond.output);
            }
            require(composed->voice.lifecycle.stages == voice.lifecycle.stages
                && composed->voice.amplitude.state().level == voice.amplitude.state().level
                && composed->voice.amplitude.state().pcmWord == voice.amplitude.state().pcmWord
                && composed->voice.pitch.pcmWord == voice.pitch.pcmWord
                && composed->voice.second.command == voice.second.command
                && composed->voice.output.tva.command == voice.output.tva.command
                && composed->voice.output.tva.level == voice.output.tva.level
                && composed->voice.prepared.pcm12 == voice.prepared.pcm12
                && composed->voice.prepared.pcm14 == voice.prepared.pcm14
                && composed->post.command == post.command && composed->post.level == post.level);
            require(preparedFirst[channel].block.wave.phase == firstVoices[channel].block.wave.phase
                && preparedSecond[channel].block.wave.phase == modulation[channel].block.wave.phase);
            previousPartPitches[channel] = composed->partPitch;
            unsigned invalidPreparationIo = 0;
            const auto noRead = [&](uint8_t) { ++invalidPreparationIo; return uint8_t(0); };
            const auto noWrite = [&](uint8_t,uint8_t) { ++invalidPreparationIo; };
            auto invalidRequest = *preparationRequests[channel];
            invalidRequest.installed.input.sample ^= 1;
            require(!sc55::PrepareNormalVoiceDsp(channel,invalidRequest,installedLifecycle,{},partControllers,
                preparedFirst,preparedSecond,preparedSources,data,conversion,waves,noRead,noWrite));
            require(!sc55::PrepareNormalVoiceDsp(24,*preparationRequests[channel],installedLifecycle,{},partControllers,
                preparedFirst,preparedSecond,preparedSources,data,conversion,waves,noRead,noWrite));
            invalidRequest = *preparationRequests[channel]; invalidRequest.installed.flags = 0;
            require(!sc55::PrepareNormalVoiceDsp(channel,invalidRequest,installedLifecycle,{},partControllers,
                preparedFirst,preparedSecond,preparedSources,data,conversion,waves,noRead,noWrite));
            require(invalidPreparationIo == 0);

            // Keep the actual pre-commit patch/sample state for a later shared
            // PCM run. This is prepared-state replay, not concurrent MIDI startup.
            concurrent.voices[channel] = voice;
            concurrent.first[channel] = firstVoices[channel];
            concurrent.firstInputs[channel] = firstInput;
            concurrent.second[channel] = modulation[channel];
            concurrent.secondSources[channel] = sources[channel];
            concurrentEntries[channel] = {channel,voice.prepared,post};

            std::array<sc55::VoiceStopState,24> lifecycle{};
            lifecycle[channel] = voice.lifecycle;
            sc55::VoiceKeyMask mask;
            sc55::PreparedVoiceBatch batch;
            const std::array<sc55::PreparedVoiceBatch::Entry,1> entries{{{channel,voice.prepared,post}}};
            require(batch.begin(entries,mask));
            auto status = batch.advance(lifecycle,mask,read,write);
            for (unsigned poll = 0; status == sc55::PreparedVoiceBatch::Status::waitingForKeyLatch && poll < 8; ++poll)
            {
                PCM_Update(*pcm,pcm->cycles+2500);
                status = batch.advance(lifecycle,mask,read,write);
            }
            require(status == sc55::PreparedVoiceBatch::Status::complete);
            require(pcm->ram2[channel][3] == voice.output.tva.command
                && uint8_t(voice.output.tva.command) == 0xba && voice.output.tva.ramp == 65535);
            const auto preparedProgress = voice.pitch.envelope.segment.progress;
            const auto preparedIncrement = voice.pitch.glide.increment;
            auto invalidCommit = lifecycle[channel];
            invalidCommit.stages[2] = 23;
            const auto originalStages = voice.lifecycle.stages;
            const auto originalAmplitude = voice.amplitude.state();
            require(!sc55::ContinueVoiceControlAfterStart(voice,invalidCommit));
            require(voice.lifecycle.stages == originalStages
                && voice.amplitude.state().segment.progress.position == originalAmplitude.segment.progress.position
                && voice.amplitude.state().pcmWord == originalAmplitude.pcmWord);
            require(sc55::ContinueVoiceControlAfterStart(voice,lifecycle[channel]));
            require(voice.pitch.envelope.stage == lifecycle[channel].stages[2]
                && voice.pitch.envelope.segment.progress.position == preparedProgress.position
                && voice.pitch.glide.increment == preparedIncrement);
            input = {};
            input.level = initialLevel;
            input.pitch = initialPitchControls;
            input.pitchReference = samplePitchReference;
            input.spatial = spatialInput;
            input.secondBypass = secondSetup.mode.bypass; input.secondTiming = secondSetup.timing;
            input.second = secondSetup.input; input.secondBase = secondSetup.controlBase;
            input.secondController = secondSetup.controller; input.secondLimit = secondSetup.limit;
            concurrent.inputs[channel] = input;
            bool rose = false, requestConsumed = false;
            bool firstMoved = false, secondMoved = false;
            sc55::ControlTaskClock controlClock;
            uint64_t controlCycle = pcm->cycles;
            const bool holdCase = (channel&1) != 0;
            const bool sostenutoCase = !holdCase && (channel&2) != 0;
            const unsigned noteOffTick = (holdCase || sostenutoCase) ? 252 : 256;
            sc55::PeriodicVoiceUpdatePass periodicPass;
            for (unsigned tick = 0; tick < 512; ++tick)
            {
                const auto duration = controlClock.untilNextExpiration();
                controlCycle += duration;
                PCM_Update(*pcm,controlCycle);
                controlClock.advance(duration);
                const auto elapsed = controlClock.consume();
                require(elapsed.has_value() && *elapsed == 1);
                input.ticks = *elapsed;
                const auto previousControllerPitch = input.pitch.offset;
                // Fixture MIDI event timing. Controller refresh runs in every pass below.
                if (tick == 32 || tick == 48 || tick == 56)
                {
                    bend(tick == 32 ? 16383 : tick == 48 ? 0 : 8192);
                    ++controllerRefreshes;
                }
                if (tick == 64) cc(11,0);
                if (tick == 96) cc(11,100);
                if (tick == 128) { cc(10,(channel&1) ? 127 : 0); cc(91,64); cc(93,32); }
                sc55::ApplyChannelOutputControls(channelControls.channel(midiChannel),input.level,input.spatial);
                if (holdCase && tick == 240) cc(64,127);
                if (sostenutoCase && tick == 240) cc(66,127);
                if (tick == noteOffTick-1 || tick == noteOffTick)
                {
                    const std::array<uint8_t,3> bytes{uint8_t(((channel&1) ? 0x90 : 0x80)|midiChannel),
                        uint8_t(tick == noteOffTick-1 ? note+1 : note),0};
                    for (const auto byte : bytes)
                        midi.push(std::span(&byte,1),[&](const auto& event) {
                            const auto result = noteState.receiveNoteOff(event,routing,{});
                            require(result && *result == (tick == noteOffTick ? uint16_t(1u<<midiChannel) : 0));
                        });
                    require(allocator.allocations[channel].releaseCommand == (tick == noteOffTick && !holdCase && !sostenutoCase ? 255 : 0));
                    allocator.publishReleaseRequests([&](uint8_t slot,uint8_t request) {
                        if (slot == channel) voice.release.pending = request;
                    });
                }
                if (sostenutoCase && tick == 256)
                {
                    require(voice.release.pending == 0 && partNoteState->sostenutoEnabled);
                    require((allocator.noteGroups[allocated->group].retirementFlags&1) == 0);
                    cc(66,0);
                    require(!partNoteState->sostenutoEnabled && allocator.allocations[channel].releaseCommand == 255);
                    for (auto key : partNoteState->retainedKeys) require(key == 255);
                    allocator.publishReleaseRequests([&](uint8_t slot,uint8_t request) {
                        if (slot == channel) voice.release.pending = request;
                    });
                    ++sostenutoReleases;
                }
                if (holdCase && tick == 256)
                {
                    require(voice.release.pending == 0 && (allocator.noteGroups[allocated->group].retirementFlags&1));
                    cc(64,0);
                    require(allocator.allocations[channel].releaseCommand == 255 && !(allocator.noteGroups[allocated->group].retirementFlags&1));
                    allocator.publishReleaseRequests([&](uint8_t slot,uint8_t request) {
                        if (slot == channel) voice.release.pending = request;
                    });
                    ++deferredReleases;
                }
                const auto firstPhase = firstVoices[channel].block.wave.phase;
                const auto secondPhase = modulation[channel].block.wave.phase;
                auto unmodulatedPitch = voice.pitch;
                auto result = sc55::VoiceControlResult::invalidInput;
                auto sent = sc55::VoicePcmUpdateResult::idle;
                std::array<uint16_t,24> stages; stages.fill(18);
                stages[channel] = voice.lifecycle.stages[0];
                const auto refreshControllers = [&](const sc55::VoiceUpdateSelection& selected) {
                    require(selected.count == 1 && selected.slots[0] == channel);
                    if (!sc55::RefreshSelectedVoiceControllers(selected,installations,partControllers,voiceControllers)) return false;
                    voiceControllers[channel].apply(input.level,input.second,input.pitch,
                        firstVoices[channel].block,modulation[channel].block);
                    return true;
                };
                const auto updateFirst = [&](unsigned slot) {
                    require(slot == channel);
                    firstVoices[slot].firstStage = voice.lifecycle.stages[0];
                    sc55::FirstVoiceModulationUpdate task;
                    using FirstResult = sc55::FirstVoiceModulationUpdate::Result;
                    auto status = task.begin(slot,firstVoices,firstInput.pitchDepth,64,modPreparation.depths.pitch);
                    if (status == FirstResult::ready)
                        status = task.resume(firstVoices,input.ticks,firstInput.pitchDepth,64,64,
                            modPreparation.depths.pitch,*data.modulationRates(),waves,read,write);
                    return status == FirstResult::updated || status == FirstResult::shared;
                };
                const auto pairedFirst = [&](unsigned,unsigned) { require(false); return false; }; // isolated partial
                const auto updateVoice = [&](unsigned slot) {
                    require(slot == channel && periodicPass.visited()[slot] == 255);
                    result = sc55::AdvanceVoiceControl(slot,voice,modulation,sources,firstVoices[slot].block,input,data,conversion,waves,read,write);
                    using Outcome = sc55::PeriodicVoiceUpdatePass::UpdateResult;
                    return result == sc55::VoiceControlResult::invalidInput ? Outcome::invalidInput
                        : result != sc55::VoiceControlResult::updated ? Outcome::skipRemaining : Outcome::proceed;
                };
                const auto writeVoice = [&](unsigned slot) {
                    require(slot == channel);
                    sent = sc55::UpdateVoicePcm(slot,voice.lifecycle,voice.prepared,voice.output,voice.second,write);
                    return sent != sc55::VoicePcmUpdateResult::invalidChannel;
                };
                periodicPass.reset();
                using PassResult = sc55::PeriodicVoiceUpdatePass::Result;
                require(periodicPass.step(stages,allocator.pcmLinks,refreshControllers,updateFirst,pairedFirst,updateVoice,writeVoice) == PassResult::updated);
                require(periodicPass.step(stages,allocator.pcmLinks,refreshControllers,updateFirst,pairedFirst,updateVoice,writeVoice) == PassResult::complete);
                for (auto visited : periodicPass.visited()) require(visited == 0);
                ++scheduledPasses;
                if ((tick == 32 || tick == 48 || tick == 56) && result == sc55::VoiceControlResult::updated)
                {
                    require(input.pitch.offset != previousControllerPitch);
                    auto level = input.level; auto second = input.second; auto pitch = input.pitch;
                    sc55::ApplyVoiceModulationOutputs(firstVoices[channel].block,modulation[channel].block,level,second,pitch);
                    pitch.offset = 0; // Same envelope/LFO/glide state, only controller offset removed.
                    require(unmodulatedPitch.advance(input.ticks,false,pitch,input.glideRate,*data.glideRates(),
                        input.pitchReference,input.correctionSource,conversion) != sc55::VoicePitchRunner::Result::invalidInput);
                    controllerPitchDifferences += unmodulatedPitch.pcmWord != voice.pitch.pcmWord;
                }
                firstMoved |= firstPhase != firstVoices[channel].block.wave.phase;
                secondMoved |= secondPhase != modulation[channel].block.wave.phase;
                require(result != sc55::VoiceControlResult::invalidInput);
                if (tick >= 64 && tick < 96 && result == sc55::VoiceControlResult::updated)
                {
                    require(voice.output.tva.level == 0);
                    ++mutedControlUpdates;
                }
                if (tick == 128 && result == sc55::VoiceControlResult::updated)
                    require(voice.output.spatial.effects == 0x0101
                        && input.spatial.pan == ((channel&1) ? 127 : 1));
                if (tick == 191 && result == sc55::VoiceControlResult::updated)
                    require(voice.output.spatial.effects == 0x4020); // CC91 high / CC93 low
                if (tick == 256) requestConsumed = voice.release.pending == 0;
                if (result != sc55::VoiceControlResult::updated) break;
                require(sent == (voice.lifecycle.stages[0] == 0 ? sc55::VoicePcmUpdateResult::idle : sc55::VoicePcmUpdateResult::written));
                if (sent == sc55::VoicePcmUpdateResult::written)
                    require(pcm->ram2[channel][0] == voice.pitch.pcmWord); // register10 maps to RAM2 index0
                rose |= pcm->ram2[channel][10] > 0;
                ++updated;
            }
            audibleLevels += rose; released += requestConsumed; ++started;
            if (!variationFixture) ++midiSelectedPartials;
            firstOscillatorsAdvanced += firstMoved; secondOscillatorsAdvanced += secondMoved;
            require(outputRange.frames > 0);
            const auto span = int64_t(outputRange.maximum)-outputRange.minimum;
            if (span > 65536) ++waveOutputs;
            if (!waveDirectory) require(outputRange.minimum == 0 && outputRange.maximum == 0);
        }
    }
    require(started == 24 && updated > 0 && audibleLevels == 24 && released == 24 && mcu->pc == 0 && mcu->cycles == 0);
    require(mutedControlUpdates == 24*32);
    require(midiSelectedPartials == 22);
    require(deferredReleases == 12);
    require(sostenutoReleases == 6);
    require(controllerRefreshes == 72);
    require(scheduledPasses >= updated && scheduledPasses > 0);
    std::printf("Native periodic pass / real DSP and PCM: %u completed passes across 24 isolated partials\n",scheduledPasses);
    require(controllerPitchDifferences > 0);
    std::printf("Native controller composition / real PCM: %u active bend refreshes across 24 partials\n",controllerRefreshes);
    std::printf("Native controller pitch: %u refreshes differ from zero-offset counterfactual\n",controllerPitchDifferences);
    std::printf("Native sostenuto / real PCM: %u retained note releases delivered\n",sostenutoReleases);
    std::printf("Native hold-off / real PCM: %u deferred note releases delivered\n",deferredReleases);
    require(allocatedSlots == 0xffffff && allocator.freeCount == 0);
    require(!allocator.createGroup({0,0,60,0,1}));
    for (unsigned slot = 0; slot < 24; ++slot) require(allocator.returnVoice(slot));
    require(allocator.freeCount == 24);
    for (const auto count : allocator.partVoiceCount) require(count == 0);
    std::puts("Native allocation / real PCM: all24 allocator-selected slots exercised and returned");
    std::printf("Native program/note selection: %u MIDI-selected partials, 2 explicit variation fixtures\n",midiSelectedPartials);
    std::printf("Native MIDI CC11 mute: %u zero-level control updates across 24 partials\n",mutedControlUpdates);
    std::printf("Data-only patch activation / real PCM: %u partials, %u updates, %u nonzero amplitude slots, %u release requests consumed (nominal 160256-cycle period)\n",
        started,updated,audibleLevels,released);
    require(!data.pitchTiming() || nativePitches == started);
    std::printf("Data-only LFO preparation: %u local pairs; phase advanced in %u first / %u second oscillators\n",
        started,firstOscillatorsAdvanced,secondOscillatorsAdvanced);
    require(firstOscillatorsAdvanced > 0 && secondOscillatorsAdvanced > 0);
    std::printf("Data-only pitch initialization: %u partials (remaining pitch fixtures: %u; task latency not modeled)\n",nativePitches,started-nativePitches);
    if (waveDirectory)
    {
        std::printf("Wave-ROM output: %u/%u partials with varying PCM audio, no control ROM execution\n",waveOutputs,started);
        require(waveOutputs == started);
    }
    else std::puts("Empty-wave negative control: all 24 partial outputs are exactly silent");
    {
        pcm = std::make_unique<pcm_t>(); PCM_Init(*pcm,*mcu); PCM_UseSimulation(*pcm,false);
        write(0x3d,0xb7); write(0x3c,0);
        if (waveDirectory)
        {
            std::copy(waveData[0].begin(),waveData[0].end(),pcm->waverom1);
            std::copy(waveData[1].begin(),waveData[1].end(),pcm->waverom2);
            std::copy(waveData[2].begin(),waveData[2].end(),pcm->waverom3);
        }
        outputRange = {}; mcu->callback_userdata = &outputRange;
        const auto preparedInstallation = installations;
        std::array<sc55::VoiceStopState,24> lifecycle;
        for (unsigned slot = 0; slot < 24; ++slot)
        {
            require(bool(concurrent.voices[slot]));
            lifecycle[slot] = concurrent.voices[slot]->lifecycle;
        }
        sc55::VoiceKeyMask mask;
        for (unsigned slot = 0; slot < 24; ++slot)
        {
            sc55::PreparedVoiceBatch batch;
            require(batch.begin(std::span(&concurrentEntries[slot],1),mask));
            auto status = batch.advance(lifecycle,mask,read,write);
            for (unsigned poll = 0; status == sc55::PreparedVoiceBatch::Status::waitingForKeyLatch && poll < 8; ++poll)
            {
                PCM_Update(*pcm,pcm->cycles+2500);
                status = batch.advance(lifecycle,mask,read,write);
            }
            require(status == sc55::PreparedVoiceBatch::Status::complete);
            require(sc55::ContinueVoiceControlAfterStart(*concurrent.voices[slot],lifecycle[slot]));
        }
        require(pcm->voice_mask == 0xffffff);
        sc55::ControlTaskClock clock;
        uint32_t voicedSlots = 0;
        unsigned peakLevels = 0, passes = 0;
        uint32_t delivered = 0, consumed = 0;
        sc55::MidiDecoder releaseMidi;
        const auto pedal = [&](uint8_t part,uint8_t value) {
            for (auto byte : std::array<uint8_t,3>{uint8_t(0xb0|part),64,value})
                releaseMidi.push(std::span(&byte,1),[&](const auto& event) {
                    require(concurrentNotes.receivePedal(event,routing));
                });
        };
        for (unsigned tick = 0; tick < 512; ++tick)
        {
            if (tick == 120)
                for (uint8_t part = 0; part < 16; ++part) pedal(part,127);
            if (tick == 128)
            {
                for (unsigned slot = 0; slot < 24; ++slot)
                {
                    const auto part = installations.voices[slot].input.part;
                    for (auto byte : std::array<uint8_t,3>{uint8_t(0x80|part),concurrentNoteKeys[slot],0})
                        releaseMidi.push(std::span(&byte,1),[&](const auto& event) {
                            require(bool(concurrentNotes.receiveNoteOff(event,routing,{})));
                        });
                }
                require(concurrent.publishNoteReleases(concurrentNotes.allocator));
                for (const auto& voice : concurrent.voices) require(voice && voice->release.pending == 0);
            }
            if (tick == 144)
            {
                for (uint8_t part = 0; part < 16; ++part) pedal(part,0);
                require(concurrent.publishNoteReleases(concurrentNotes.allocator));
                for (unsigned slot = 0; slot < 24; ++slot)
                    if (concurrent.voices[slot]->release.pending) delivered |= 1u<<slot;
                require(delivered == 0xffffff);
            }
            const auto duration = clock.untilNextExpiration();
            PCM_Update(*pcm,pcm->cycles+duration); clock.advance(duration);
            const auto elapsed = clock.consume(); require(elapsed && *elapsed == 1);
            const auto updatedMask = concurrent.advance(*elapsed,installations,partControllers,concurrentNotes.allocator,data,conversion,waves,read,write);
            require(bool(updatedMask));
            if (tick == 0) require(*updatedMask == 0xffffff);
            unsigned nonzero = 0;
            for (unsigned slot = 0; slot < 24; ++slot)
            {
                if (tick >= 144 && concurrent.voices[slot]->release.pending == 0) consumed |= 1u<<slot;
                if (pcm->ram2[slot][10] > 0) { voicedSlots |= 1u<<slot; ++nonzero; }
                if (concurrent.lastWrites[slot] == sc55::VoicePcmUpdateResult::written)
                    require(pcm->ram2[slot][0] == concurrent.voices[slot]->pitch.pcmWord);
            }
            peakLevels = std::max(peakLevels,nonzero); ++passes;
        }
        require(voicedSlots == 0xffffff && peakLevels == 24);
        require(consumed == 0xffffff);
        require(outputRange.frames > 0 && mcu->pc == 0 && mcu->cycles == 0);
        if (waveDirectory) require(int64_t(outputRange.maximum)-outputRange.minimum > 65536);
        else require(outputRange.minimum == 0 && outputRange.maximum == 0);
        std::printf("Native real-patch concurrency: %u passes, %u simultaneously nonzero levels, %llu frames, %s\n",
            passes,peakLevels,static_cast<unsigned long long>(outputRange.frames),waveDirectory ? "varying waveform-ROM audio" : "empty-wave silence");
        std::puts("Native concurrent MIDI release: 24 held note-offs deferred, delivered on CC64 off, and consumed");
        unsigned tailPasses = 0;
        while (concurrentNotes.allocator.freeCount != 24 && tailPasses < 8192)
        {
            const auto duration = clock.untilNextExpiration();
            PCM_Update(*pcm,pcm->cycles+duration); clock.advance(duration);
            const auto elapsed = clock.consume(); require(bool(elapsed));
            require(bool(concurrent.advance(*elapsed,installations,partControllers,concurrentNotes.allocator,
                data,conversion,waves,read,write)));
            ++tailPasses;
        }
        require(concurrentNotes.allocator.freeCount == 24);
        for (unsigned slot = 0; slot < 24; ++slot)
        {
            require(concurrent.voices[slot]->lifecycle.stages[0] == 22);
            require(concurrentNotes.allocator.activity[slot] == 0);
            require(concurrentNotes.allocator.pcmLinks.first[slot] == 255
                && concurrentNotes.allocator.pcmLinks.second[slot] == 255);
        }
        std::printf("Native concurrent termination: all24 slots returned after %u additional tail passes\n",tailPasses);
        uint32_t reallocated = 0;
        unsigned changedPreparedPitches = 0;
        const auto previousCycles = pcm->cycles;
        outputRange = {};
        for (unsigned note = 0; note < 24; ++note)
        {
            const auto next = concurrentNotes.allocator.freeHead;
            require(next < 24);
            auto input = preparedInstallation.voices[next].input;
            sc55::ChannelControls selectionControls;
            sc55::MidiDecoder noteDecoder;
            std::optional<sc55::PatchVelocityPlan> velocity;
            const auto program = preparationPrograms[next];
            const std::array<uint8_t,5> message{uint8_t(0xc0|input.part),uint8_t(program < 128 ? program : 0),
                uint8_t(0x90|input.part),64,100};
            for (auto byte : message)
                noteDecoder.push(std::span(&byte,1),[&](const auto& event) {
                    if (selectionControls.apply(event)) return;
                    require(sc55::AcceptNotePart(event.status&15,input.part,routing[input.part],{}));
                    if (program < 128)
                    {
                        const auto selected = sc55::PrepareMelodicNoteVelocity(event,selectionControls.channel(input.part),0,false,0,data);
                        require(selected && selected->tone == input.tone);
                        velocity = selected->partials; input.originalKey = selected->note; input.velocity = selected->velocity;
                    }
                    else
                    {
                        // Existing explicit variation-tone fixture, not a GM fallback.
                        const auto& patch = *data.patch(input.tone);
                        velocity = sc55::PreparePatchVelocity(patch.common[6],event.second,
                            patch.partial[0].raw,patch.partial[1].raw,0,false,*data.curves());
                        input.originalKey = event.first; input.velocity = event.second;
                    }
                });
            require(velocity && velocity->partials[input.partial]);
            const auto key = input.originalKey;
            const auto& patch = *data.patch(input.tone);
            const auto sample = sc55::PreparePartialSample(patch.partial[input.partial],*data.samples(),
                key,key,key,scale,0,0,concurrentNoteKeys[next]);
            require(sample && !(sample->sampleId&0x8000));
            input.sample = sample->sampleId;
            input.originalKey = sample->key.storedOriginalNote.value_or(key);
            input.adjustedKey = sample->key.storedAdjustedKey.value_or(key);
            input.sampleKey = sample->key.lookupKey;
            auto& request = *preparationRequests[next];
            request.sample = *sample; request.amplitude = velocity->partials[input.partial]->amplitude;
            request.secondary = velocity->partials[input.partial]->secondary;
            request.sourceKey = concurrentNoteKeys[next]; request.fractionalTune = sample->pitch.fraction;
            concurrentNoteKeys[next] = key;
            const auto group = concurrentNotes.allocator.createGroup({input.part,0x80,key,1,1});
            require(bool(group) && group->voices[0] < 24);
            const auto slot = group->voices[0]; require(slot == next);
            const auto bit = 1u<<slot;
            require((reallocated&bit) == 0); reallocated |= bit;
            auto flags = preparedInstallation.voices[slot].flags;
            lifecycle[slot] = concurrent.voices[slot]->lifecycle;
            require(sc55::RestartAndInstallVoice(slot,input,flags,concurrentNotes.allocator,
                installations,lifecycle[slot],read,write));
            const auto task = sc55::DispatchNextVoiceTask(lifecycle,concurrentNotes.allocator.pcmLinks,
                concurrentNotes.allocator.activity);
            require(task && task->kind == sc55::VoiceTaskDispatch::Kind::prepare
                && task->count == 1 && task->slots[0] == slot);
            previousPartPitches[slot].glide = concurrent.voices[slot]->pitch.glide;
            preparationRequests[slot]->installed = installations.voices[slot];
            const auto prepared = sc55::PrepareNormalVoiceDsp(slot,*preparationRequests[slot],lifecycle[slot],
                previousPartPitches[slot],partControllers,concurrent.first,concurrent.second,concurrent.secondSources,
                data,conversion,waves,read,write);
            require(bool(prepared));
            changedPreparedPitches += prepared->partPitch.values.pitch != previousPartPitches[slot].values.pitch;
            concurrent.voices[slot] = prepared->voice;
            concurrent.inputs[slot] = prepared->controls;
            concurrent.firstInputs[slot] = prepared->firstControls;
            previousPartPitches[slot] = prepared->partPitch;
            concurrentEntries[slot] = {slot,prepared->voice.prepared,prepared->post};
            lifecycle[slot] = concurrent.voices[slot]->lifecycle;
            sc55::PreparedVoiceBatch batch;
            require(batch.begin(std::span(&concurrentEntries[slot],1),mask));
            auto status = batch.advance(lifecycle,mask,read,write);
            for (unsigned poll = 0; (status == sc55::PreparedVoiceBatch::Status::waitingForReuse
                || status == sc55::PreparedVoiceBatch::Status::waitingForKeyLatch) && poll < 1024; ++poll)
            {
                PCM_Update(*pcm,pcm->cycles+2500);
                status = batch.advance(lifecycle,mask,read,write);
            }
            require(status == sc55::PreparedVoiceBatch::Status::complete);
            require(sc55::ContinueVoiceControlAfterStart(*concurrent.voices[slot],lifecycle[slot]));
        }
        require(reallocated == 0xffffff && concurrentNotes.allocator.freeCount == 0);
        require(changedPreparedPitches > 0);
        require(mask.enabled == 0xffffff && mask.prepared == 0 && pcm->cycles > previousCycles);
        uint32_t secondVoiced = 0;
        unsigned secondPasses = 0;
        for (; secondPasses < 8192; ++secondPasses)
        {
            // Ignore any old-generation residual during activation when
            // asserting new audio; this resets only measurement, not the PCM.
            if (secondPasses == 32) outputRange = {};
            if (secondPasses == 144)
            {
                for (unsigned slot = 0; slot < 24; ++slot)
                {
                    const auto part = installations.voices[slot].input.part;
                    for (auto byte : std::array<uint8_t,3>{uint8_t(0x80|part),concurrentNoteKeys[slot],0})
                        releaseMidi.push(std::span(&byte,1),[&](const auto& event) {
                            require(bool(concurrentNotes.receiveNoteOff(event,routing,{})));
                        });
                }
                require(concurrent.publishNoteReleases(concurrentNotes.allocator));
            }
            const auto duration = clock.untilNextExpiration();
            PCM_Update(*pcm,pcm->cycles+duration); clock.advance(duration);
            const auto elapsed = clock.consume(); require(bool(elapsed));
            require(bool(concurrent.advance(*elapsed,installations,partControllers,concurrentNotes.allocator,
                data,conversion,waves,read,write)));
            for (unsigned slot = 0; slot < 24; ++slot)
                if (concurrent.lastWrites[slot] == sc55::VoicePcmUpdateResult::written)
                {
                    require(pcm->ram2[slot][0] == concurrent.voices[slot]->pitch.pcmWord);
                    if (pcm->ram2[slot][9] > 0 && pcm->ram2[slot][10] > 0) secondVoiced |= 1u<<slot;
                }
            if (secondPasses > 144 && concurrentNotes.allocator.freeCount == 24) break;
        }
        require(secondVoiced == 0xffffff && concurrentNotes.allocator.freeCount == 24);
        require(outputRange.frames > 0 && mcu->pc == 0 && mcu->cycles == 0);
        if (waveDirectory) require(int64_t(outputRange.maximum)-outputRange.minimum > 65536);
        else require(outputRange.minimum == 0 && outputRange.maximum == 0);
        std::printf("Native second-generation playback: all24 slots reactivated and returned in %u passes, without PCM/allocator/mask reset\n",secondPasses+1);
        std::printf("Native fresh DSP preparation: 24 byte-decoded key64 notes, %u changed base pitches; no saved DSP-state replay\n",changedPreparedPitches);
        // All capital programs selecting both partials at velocity100.
        std::vector<unsigned> pairPrograms;
        for (unsigned program = 0; program < 128; ++program)
        {
            const auto tone = sc55::ResolveV121MelodicPreset(0,program);
            require(tone && data.patch(*tone));
            const auto& patch = *data.patch(*tone);
            const auto velocity = sc55::PreparePatchVelocity(patch.common[6],100,
                patch.partial[0].raw,patch.partial[1].raw,0,false,*data.curves());
            if (velocity.candidates.count == 2) pairPrograms.push_back(program);
        }
        require(!pairPrograms.empty());
        sc55::NativeVoiceEngine engine;
        engine.clock = clock; engine.mask = mask;
        for (const bool combinedStart : {false,true})
        for (const auto pairProgram : pairPrograms)
        {
            engine.runtime = {}; engine.installation = {}; engine.notes = {};
            auto& paired = engine.runtime;
            auto& pairInstallation = engine.installation;
            auto& pairNotes = engine.notes; pairNotes.allocator = concurrentNotes.allocator;
            auto& lifecycle = engine.lifecycle;
            auto& clock = engine.clock;
            auto& mask = engine.mask;
            for (unsigned slot = 0; slot < 24; ++slot)
            {
                paired.first[slot].firstStage = paired.second[slot].firstStage = 22;
                paired.first[slot].block.rateIndex = uint8_t(17+slot);
                lifecycle[slot] = concurrent.voices[slot]->lifecycle;
            }
            sc55::ChannelControls pairChannels;
            sc55::MidiEventQueue<13> startMidi;
            const std::array<uint8_t,2> programMessage{0xc0,uint8_t(pairProgram)};
            require(startMidi.push(programMessage,pcm->cycles).consumed == programMessage.size());
            for (unsigned note = 0; note < 12; ++note)
            {
                const std::array<uint8_t,3> message{0x90,uint8_t(60+note),uint8_t(combinedStart ? 98 : 100)};
                require(startMidi.push(message,pcm->cycles).consumed == message.size());
            }
            require(paired.serviceMidi(startMidi,pcm->cycles,[&](const auto& event) {
                require(pairChannels.apply(event)); return sc55::MidiDispatchResult::accepted;
            }) == sc55::MidiDispatchResult::accepted);
            uint32_t pairedAllocated = 0;
            unsigned deliveredStarts = 0;
            for (unsigned note = 0; note < 12; ++note)
            {
                std::optional<sc55::MelodicNoteVelocity> selection;
                std::array<sc55::NormalVoicePreparationEntry,2> entries;
                std::optional<sc55::PreparedNormalVoiceBatch> prepared;
                unsigned invalidIo = 0;
                const auto forbiddenRead = [&](uint8_t) { ++invalidIo; return uint8_t(0); };
                const auto forbiddenWrite = [&](uint8_t,uint8_t) { ++invalidIo; };
                require(paired.serviceMidi(startMidi,pcm->cycles,[&](const auto& event) {
                    require(event.status == 0x90 && event.first == 60+note
                        && event.second == (combinedStart ? 98 : 100));
                    ++deliveredStarts;
                    if (combinedStart)
                    {
                        const auto key = event.first;
                        const sc55::PartialSampleInstallInputs sampleInput{scale,key,key,key,0,0,key,0,160};
                        const sc55::NormalPartialDspInputs dspInput{key,false,0,preparationRequests[0]->controls,{},{}};
                        using Start = sc55::VoiceControlRuntime::MelodicStartResult::Status;
                        if (note == 0)
                            for (unsigned scenario = 0; scenario < 8; ++scenario)
                            {
                                auto probe = paired; auto probeAllocator = pairNotes.allocator;
                                auto probeInstall = pairInstallation; auto probeLife = lifecycle; auto probeMask = mask;
                                auto probeEvent = event;
                                sc55::MelodicAllocationInputs probeAllocation{0,false,0,0,0x80,1};
                                std::array<sc55::PartialSampleInstallInputs,2> probeSamples{sampleInput,sampleInput};
                                if (scenario == 0) probeEvent.second = 0;
                                if (scenario == 1) require(probeAllocator.initializeTables(1));
                                if (scenario == 2) probeLife[0].fieldCAF4 = 4;
                                if (scenario == 3) probeSamples[1].initialKey = 128;
                                if (scenario == 4) probeLife[0].fieldCAF4 = 3;
                                if (scenario == 5) probeAllocation.keyRange = {uint8_t(event.first+1),127};
                                if (scenario == 6)
                                {
                                    probeEvent.second = 1; probeAllocation.velocityAdjustment = {1,64};
                                    probeAllocation.keyRange = {127,0}; // Zero rejection precedes key range.
                                }
                                if (scenario == 7) probeAllocation.velocityAdjustment = {128,64};
                                const auto beforeAllocator = probeAllocator;
                                const auto result = probe.startRoutedMelodicNote(probeEvent,pairChannels.channel(0),
                                    probeAllocation,probeSamples,{dspInput,dspInput},probeAllocator,
                                    probeInstall,probeLife,probeMask,partControllers,data,conversion,waves,forbiddenRead,forbiddenWrite);
                                const std::array<Start,8> expected{Start::invalidInput,Start::needsCapacity,
                                    Start::deferred,Start::failed,Start::failed,Start::keyRangeRejected,
                                    Start::velocityRejected,Start::invalidInput};
                                require(result.status == expected[scenario] && invalidIo == 0);
                                if (scenario != 3) require(std::memcmp(&probeAllocator,&beforeAllocator,sizeof(probeAllocator)) == 0);
                                if (scenario >= 5) require(!probe.failed() && !probe.startupPending()
                                    && !result.requests && !result.prepared);
                                if (scenario == 3 || scenario == 4)
                                {
                                    require(probe.failed()); const auto remaining = probeAllocator.freeCount;
                                    require(probe.startRoutedMelodicNote(event,pairChannels.channel(0),
                                        {0,false,0,0,0x80,1},{sampleInput,sampleInput},{dspInput,dspInput},probeAllocator,
                                        probeInstall,probeLife,probeMask,partControllers,data,conversion,waves,
                                        forbiddenRead,forbiddenWrite).status == Start::failed);
                                    require(probeAllocator.freeCount == remaining && invalidIo == 0);
                                }
                            }
                        if(note==0) {
                            // The production installer owns a suspended note,
                            // not pointers into the caller's transient inputs.
                            auto staged=engine;
                            const auto allocated=sc55::AllocateMelodicNote(event,pairChannels.channel(0),
                                {0,false,0,0,0x80,1},data,staged.notes.allocator);
                            require(allocated.status==sc55::MelodicAllocationResult::Status::allocated);
                            auto immediate=staged;
                            std::array<sc55::NormalPartialDspInputs,2> ownedDsp{dspInput,dspInput};
                            std::vector<std::pair<uint8_t,uint8_t>> expectedWrites,actualWrites;
                            const auto zeroRead=[](uint8_t) { return uint8_t(0); };
                            const auto expectedWrite=[&](uint8_t a,uint8_t v) { expectedWrites.emplace_back(a,v); };
                            const auto actualWrite=[&](uint8_t a,uint8_t v) { actualWrites.emplace_back(a,v); };
                            const auto expected=immediate.runtime.beginAllocatedNote(allocated,0,
                                {sampleInput,sampleInput},ownedDsp,immediate.notes.allocator,immediate.installation,
                                immediate.lifecycle,immediate.mask,partControllers,data,conversion,waves,zeroRead,expectedWrite);
                            require(expected.status==Start::started);
                            require(staged.runtime.beginControlPass(3));
                            const auto progress=staged.runtime.beginAllocatedNote(allocated,0,
                                {sampleInput,sampleInput},ownedDsp,staged.notes.allocator,staged.installation,
                                staged.lifecycle,staged.mask,partControllers,data,conversion,waves,zeroRead,actualWrite,
                                sc55::VoiceControlRuntime::PreparationSlice::partial);
                            require(progress.status==Start::preparing && staged.runtime.preparationPending()
                                && staged.runtime.startupPending() && !progress.requests && !progress.prepared);
                            const auto before=staged.notes.allocator;
                            const auto written=actualWrites.size();
                            require(staged.runtime.beginAllocatedNote(allocated,0,{sampleInput,sampleInput},ownedDsp,
                                staged.notes.allocator,staged.installation,staged.lifecycle,staged.mask,
                                partControllers,data,conversion,waves,forbiddenRead,forbiddenWrite).status==Start::deferred);
                            require(!staged.stopGroup(0,allocated.group->group,forbiddenRead,forbiddenWrite));
                            require(!staged.releaseMonoNote(0,event.first));
                            require(staged.resumeControl(partControllers,data,conversion,waves,
                                forbiddenRead,forbiddenWrite).status==sc55::VoiceControlRuntime::ControlProgress::deferred);
                            staged.clock.advance(staged.clock.untilNextExpiration());
                            require(staged.runtime.serviceControl(staged.clock,staged.installation,partControllers,
                                staged.notes.allocator,data,conversion,waves,forbiddenRead,forbiddenWrite).status
                                ==sc55::VoiceControlRuntime::ScheduledStatus::deferred && staged.clock.ready());
                            require(staged.runtime.serviceStopTask(staged.lifecycle,staged.notes.allocator).status
                                ==sc55::VoiceControlRuntime::StopTaskStatus::deferred);
                            staged.pollStart(forbiddenRead,forbiddenWrite);
                            require(!staged.handlePcmBoundary(0,conversion,forbiddenRead,forbiddenWrite));
                            require(staged.runtime.preparationPending() && actualWrites.size()==written && invalidIo==0
                                && std::memcmp(&before,&staged.notes.allocator,sizeof before)==0);
                            ownedDsp[1].sourceKey=127;
                            sc55::VoiceControlRuntime::MelodicStartResult completed{Start::preparing,{},{}};
                            for(unsigned step=0;step<4;++step) {
                                completed=staged.runtime.resumeNotePreparation(staged.notes.allocator,
                                    staged.installation,staged.lifecycle,staged.mask,partControllers,data,conversion,waves,
                                    zeroRead,actualWrite);
                                if(step<3) {
                                    require(completed.status==Start::preparing && !completed.requests && !completed.prepared
                                        && staged.runtime.preparationPending() && staged.mask.prepared==0);
                                    require(staged.runtime.beginAllocatedNote(allocated,0,{sampleInput,sampleInput},ownedDsp,
                                        staged.notes.allocator,staged.installation,staged.lifecycle,staged.mask,
                                        partControllers,data,conversion,waves,forbiddenRead,forbiddenWrite).status==Start::deferred);
                                    require(staged.resumeControl(partControllers,data,conversion,waves,
                                        forbiddenRead,forbiddenWrite).status==sc55::VoiceControlRuntime::ControlProgress::deferred);
                                }
                            }
                            require(completed.status==Start::started && !staged.runtime.preparationPending()
                                && staged.runtime.startupPending() && staged.runtime.controlPending());
                            require(completed.requests && completed.requests->count==expected.requests->count
                                && completed.requests->entries[1].request.sourceKey==dspInput.sourceKey);
                            require(actualWrites==expectedWrites
                                && std::memcmp(&staged.notes.allocator,&immediate.notes.allocator,sizeof before)==0);
                            require(staged.runtime.resumeNotePreparation(staged.notes.allocator,staged.installation,
                                staged.lifecycle,staged.mask,partControllers,data,conversion,waves,
                                forbiddenRead,forbiddenWrite).status==Start::invalidInput && invalidIo==0);
                        }
                        const auto started = engine.startRoutedMelodicNote(event,pairChannels.channel(0),
                            {0,false,0,0,0x80,1,{},0,{64,65}},{sampleInput,sampleInput},{dspInput,dspInput},
                            partControllers,data,conversion,waves,read,write);
                        require(started.status == Start::started && started.requests && started.prepared);
                        require(started.requests->count == 2 && started.prepared->count == 2);
                        entries = started.requests->entries; prepared = started.prepared;
                        for (const auto& entry : entries)
                            require(entry.request.installed.input.velocity == 100);
                        for (const auto& entry : entries)
                        { require(!(pairedAllocated&(1u<<entry.slot))); pairedAllocated |= 1u<<entry.slot; }
                        const auto remaining = pairNotes.allocator.freeCount;
                        require(paired.startRoutedMelodicNote(event,pairChannels.channel(0),
                            {0,false,0,0,0x80,1},{sampleInput,sampleInput},{dspInput,dspInput},pairNotes.allocator,
                            pairInstallation,lifecycle,mask,partControllers,data,conversion,waves,forbiddenRead,forbiddenWrite).status
                            == Start::deferred);
                        require(pairNotes.allocator.freeCount == remaining && invalidIo == 0);
                        return sc55::MidiDispatchResult::accepted;
                    }
                    const auto allocation = sc55::AllocateMelodicNote(event,pairChannels.channel(0),
                        {0,false,0,0,0x80,1},data,pairNotes.allocator);
                    require(allocation.status == sc55::MelodicAllocationResult::Status::allocated);
                    selection = allocation.selection;
                    require(selection && selection->partials.candidates.count == 2);
                    const auto& patch = *data.patch(selection->tone);
                    require(allocation.group && allocation.dispatch[0].prepare && allocation.dispatch[1].prepare);
                    const auto key = selection->note;
                    const sc55::PartialSampleInstallInputs sampleInput{scale,key,key,key,0,0,key,0,160};
                    const std::array<sc55::PartialSampleInstallInputs,2> sampleInputs{sampleInput,sampleInput};
                    auto invalidSampleInputs = sampleInputs; invalidSampleInputs[1].initialKey = 128;
                    const auto beforeStatus = SnapshotAllocationField(pairNotes.allocator,&sc55::VoiceAllocator::VoiceAllocation::status);
                    const auto beforeRelease = SnapshotAllocationField(pairNotes.allocator,&sc55::VoiceAllocator::VoiceAllocation::releaseCommand);
                    require(!sc55::PrepareAndInstallMelodicSamples(allocation,0,invalidSampleInputs,data,
                        pairNotes.allocator,pairInstallation,lifecycle,forbiddenRead,forbiddenWrite));
                    require(invalidIo == 0 && SnapshotAllocationField(pairNotes.allocator,&sc55::VoiceAllocator::VoiceAllocation::status) == beforeStatus
                        && SnapshotAllocationField(pairNotes.allocator,&sc55::VoiceAllocator::VoiceAllocation::releaseCommand) == beforeRelease);
                    const auto samples = sc55::PrepareAndInstallMelodicSamples(allocation,0,sampleInputs,data,
                        pairNotes.allocator,pairInstallation,lifecycle,read,write);
                    require(samples && (*samples)[0] && (*samples)[1]);
                    for (unsigned partial = 0; partial < 2; ++partial)
                    {
                        const auto& installedSample = *(*samples)[partial];
                        const auto slot = installedSample.slot;
                        require(slot < 24 && !(pairedAllocated&(1u<<slot))); pairedAllocated |= 1u<<slot;
                        const auto& sample = installedSample.sample;
                        require(installedSample.installed && !(sample.sampleId&0x8000));
                        const auto referenceSample = sc55::PreparePartialSample(patch.partial[partial],*data.samples(),key,key,key,scale,0,0,key);
                        require(referenceSample && referenceSample->sampleId == sample.sampleId
                            && referenceSample->pitch.fraction == sample.pitch.fraction);
                    }
                    auto referenceLifecycle = lifecycle; auto referenceActivity = pairNotes.allocator.activity;
                    const auto task = sc55::DispatchNextVoiceTask(referenceLifecycle,pairNotes.allocator.pcmLinks,referenceActivity);
                    require(task && task->kind == sc55::VoiceTaskDispatch::Kind::prepare && task->count == 2);
                    const sc55::NormalPartialDspInputs dspInput{key,false,0,preparationRequests[0]->controls,{},{}};
                    const std::array<sc55::NormalPartialDspInputs,2> dspInputs{dspInput,dspInput};
                    unsigned otherSlot = 0;
                    while (otherSlot == (*samples)[0]->slot || otherSlot == (*samples)[1]->slot) ++otherSlot;
                    for (uint8_t otherTask : {uint8_t(2),uint8_t(4)})
                    {
                        auto otherLifecycle = lifecycle; auto otherActivity = pairNotes.allocator.activity;
                        for (auto& voice : otherLifecycle) voice.fieldCAF4 = 0;
                        otherLifecycle[otherSlot].fieldCAF4 = otherTask;
                        otherLifecycle[otherSlot].stages.fill(18); otherActivity[otherSlot] = 123;
                        sc55::VoiceLinks otherLinks;
                        require(!sc55::DispatchNormalVoiceInputs(*selection,*samples,dspInputs,
                            otherLifecycle,otherLinks,otherActivity));
                        require(otherLifecycle[otherSlot].fieldCAF4 == otherTask
                            && otherLifecycle[otherSlot].stages[0] == 18 && otherActivity[otherSlot] == 123);
                    }
                    auto wrongSamples = *samples;
                    wrongSamples[1]->installed->input.sample ^= 1;
                    require(!sc55::DispatchNormalVoiceInputs(*selection,wrongSamples,dspInputs,lifecycle,
                        pairNotes.allocator.pcmLinks,pairNotes.allocator.activity));
                    for (const auto& sample : *samples) require(lifecycle[sample->slot].fieldCAF4 == 2);
                    const auto dispatched = sc55::DispatchNormalVoiceInputs(*selection,*samples,dspInputs,lifecycle,
                        pairNotes.allocator.pcmLinks,pairNotes.allocator.activity);
                    require(dispatched && dispatched->count == 2 && referenceActivity == pairNotes.allocator.activity);
                    entries = dispatched->entries;
                    for (unsigned i = 0; i < 2; ++i)
                    {
                        require(entries[i].slot == task->slots[i] && entries[i].lifecycle.fieldCAF4 == 0);
                        const auto partial = entries[i].request.installed.input.partial;
                        require(entries[i].request.amplitude == selection->partials.partials[partial]->amplitude
                            && entries[i].request.secondary == selection->partials.partials[partial]->secondary
                            && entries[i].request.fractionalTune == (*samples)[partial]->sample.pitch.fraction);
                    }
                    auto invalidEntries = entries; invalidEntries[1].request.installed.input.sample ^= 1;
                    require(!sc55::PrepareNormalVoicesDsp(invalidEntries,partControllers,paired.first,paired.second,
                        paired.secondSources,data,conversion,waves,forbiddenRead,forbiddenWrite));
                    invalidEntries = entries; invalidEntries[1].slot = invalidEntries[0].slot;
                    require(!sc55::PrepareNormalVoicesDsp(invalidEntries,partControllers,paired.first,paired.second,
                        paired.secondSources,data,conversion,waves,forbiddenRead,forbiddenWrite));
                    require(invalidIo == 0 && paired.first[entries[0].slot].block.rateIndex == 17+entries[0].slot);
                    require(!paired.prepareAndBeginNormalStart(invalidEntries,lifecycle,mask,partControllers,
                        data,conversion,waves,forbiddenRead,forbiddenWrite));
                    require(!paired.failed() && invalidIo == 0);
                    auto rejectedStart = paired; auto rejectedLifecycle = lifecycle; auto rejectedMask = mask;
                    invalidEntries = entries; invalidEntries[1].request.installed.input.sample ^= 1;
                    require(!rejectedStart.prepareAndBeginNormalStart(invalidEntries,rejectedLifecycle,rejectedMask,
                        partControllers,data,conversion,waves,forbiddenRead,forbiddenWrite));
                    require(rejectedStart.failed() && !rejectedStart.startupPending() && rejectedMask.prepared == mask.prepared);
                    require(!rejectedStart.prepareAndBeginNormalStart(entries,rejectedLifecycle,rejectedMask,
                        partControllers,data,conversion,waves,forbiddenRead,forbiddenWrite));
                    require(invalidIo == 0);
                    if(note==0) {
                        auto whole=paired,staged=paired;
                        auto wholeLife=lifecycle,stagedLife=lifecycle;
                        auto wholeMask=mask,stagedMask=mask;
                        auto transient=entries;
                        std::vector<std::pair<uint8_t,uint8_t>> wholeWrites,stagedWrites;
                        const auto load=[](uint8_t) {return uint8_t(0);};
                        const auto wholeWrite=[&](uint8_t a,uint8_t v) {wholeWrites.emplace_back(a,v);};
                        const auto stagedWrite=[&](uint8_t a,uint8_t v) {stagedWrites.emplace_back(a,v);};
                        const auto expected=whole.prepareAndBeginNormalStart(entries,wholeLife,wholeMask,
                            partControllers,data,conversion,waves,load,wholeWrite);
                        require(bool(expected));
                        require(staged.beginNormalPreparation(transient,stagedMask,partControllers,data));
                        require(staged.preparationPending() && staged.startupPending());
                        require(!staged.beginNormalPreparation(entries,stagedMask,partControllers,data));
                        transient={}; // Nothing borrowed from the caller survives begin.
                        using Progress=sc55::NormalVoiceDspPreparation::Progress;
                        for(unsigned phase=0;phase<3;++phase) {
                            const auto step=staged.resumeNormalPreparation(stagedLife,stagedMask,
                                data,conversion,waves,load,stagedWrite);
                            require(step.status==(phase<2 ? Progress::advanced : Progress::complete));
                            require(staged.preparationPending()==(phase<2));
                            if(phase<2) require(!step.prepared && stagedMask.prepared==0);
                            else require(step.prepared && step.prepared->count==expected->count);
                        }
                        require(stagedWrites==wholeWrites && stagedMask.prepared==wholeMask.prepared
                            && staged.startupPending());
                        const auto written=stagedWrites.size();
                        require(staged.resumeNormalPreparation(stagedLife,stagedMask,data,conversion,waves,
                            load,stagedWrite).status==Progress::failed && stagedWrites.size()==written);
                    }
                    prepared = paired.prepareAndBeginNormalStart(entries,lifecycle,mask,partControllers,
                        data,conversion,waves,read,write);
                    require(prepared && prepared->count == 2);
                    return sc55::MidiDispatchResult::accepted;
                }) == sc55::MidiDispatchResult::accepted);
                require(deliveredStarts == note+1 && startMidi.size() == 11-note);
                const auto firstSlot = entries[0].slot, secondSlot = entries[1].slot;
                require(paired.first[firstSlot].block.wave.phase == paired.first[secondSlot].block.wave.phase
                    && paired.first[firstSlot].sharing.source == paired.first[secondSlot].sharing.source);
                require(paired.first[secondSlot].block.rateIndex == 17+secondSlot);
                const std::array<uint8_t,2> startSlots{uint8_t(firstSlot),uint8_t(secondSlot)};
                const std::array<uint8_t,2> duplicateSlots{uint8_t(firstSlot),uint8_t(firstSlot)};
                require(!paired.beginPreparedStart(duplicateSlots,*prepared,lifecycle,mask));
                require(paired.startupPending());
                require(paired.serviceStopTask(lifecycle,pairNotes.allocator).status
                    == sc55::VoiceControlRuntime::StopTaskStatus::deferred);
                require(!paired.prepareAndBeginNormalStart(entries,lifecycle,mask,partControllers,
                    data,conversion,waves,forbiddenRead,forbiddenWrite));
                require(paired.serviceMidi(startMidi,pcm->cycles,[&](const auto&) {
                    ++deliveredStarts; return sc55::MidiDispatchResult::accepted;
                }) == sc55::MidiDispatchResult::deferred);
                require(deliveredStarts == note+1 && startMidi.size() == 11-note && invalidIo == 0);
                sc55::MidiEventQueue<2> pendingMidi;
                const std::array<uint8_t,5> pendingPedals{0xb0,64,127,64,0};
                require(pendingMidi.push(pendingPedals,pcm->cycles).consumed == pendingPedals.size());
                unsigned prematureMidi = 0;
                require(paired.serviceMidi(pendingMidi,pcm->cycles,[&](auto) {
                    ++prematureMidi; return sc55::MidiDispatchResult::accepted;
                }) == sc55::MidiDispatchResult::deferred);
                require(prematureMidi == 0 && pendingMidi.size() == 2);
                const auto beforeRelease = SnapshotAllocationField(pairNotes.allocator,&sc55::VoiceAllocator::VoiceAllocation::releaseCommand);
                require(engine.receiveReleaseMidi({sc55::MidiDecoder::Kind::message,0xb0,64,127,2},routing,{}).status
                    == sc55::NativeVoiceEngine::ReleaseMidiResult::Status::deferred);
                require(SnapshotAllocationField(pairNotes.allocator,&sc55::VoiceAllocator::VoiceAllocation::releaseCommand) == beforeRelease);
                require(!paired.beginPreparedStart(startSlots,*prepared,lifecycle,mask));
                require(!paired.advance(1,pairInstallation,partControllers,pairNotes.allocator,
                    data,conversion,waves,forbiddenRead,forbiddenWrite));
                require(!paired.publishNoteReleases(pairNotes.allocator));
                require(!paired.failed() && invalidIo == 0);
                using Scheduled = sc55::VoiceControlRuntime::ScheduledStatus;
                sc55::ControlTaskClock deferredClock;
                const auto period = deferredClock.untilNextExpiration();
                deferredClock.advance(uint64_t(period)*3+17);
                // Reuse wait excludes the two destinations, not existing
                // periodic owners. Key-latch protection is checked below.
                const auto duringReuse=paired.serviceControl(deferredClock,pairInstallation,partControllers,pairNotes.allocator,
                    data,conversion,waves,read,write);
                require(duringReuse.status==Scheduled::updated && duringReuse.elapsed==3
                    && !(duringReuse.updatedMask&((1u<<firstSlot)|(1u<<secondSlot))));
                auto observedClock = deferredClock;
                require(!observedClock.consume() && deferredClock.untilNextExpiration() == period-17);
                deferredClock.advance(uint64_t(period)*256);
                const auto wrappedReuse=paired.serviceControl(deferredClock,pairInstallation,partControllers,pairNotes.allocator,
                    data,conversion,waves,read,write);
                require(wrappedReuse.status==Scheduled::updated && wrappedReuse.elapsed==0
                    && !(wrappedReuse.updatedMask&((1u<<firstSlot)|(1u<<secondSlot))));
                observedClock = deferredClock;
                require(!observedClock.consume() && invalidIo == 0);
                require(!paired.voices[firstSlot] && !paired.voices[secondSlot]);
                // A stalled device gets exactly one bounded poll, no spin and
                // no publication. A cancelled startup latches failure and must
                // not replay partial device mutation on a subsequent call.
                auto stalled = paired; auto stalledLifecycle = lifecycle; auto stalledMask = mask;
                unsigned stalledReads = 0, stalledWrites = 0;
                const auto stalledRead = [&](uint8_t) { ++stalledReads; return uint8_t(1); };
                const auto stalledWrite = [&](uint8_t,uint8_t) { ++stalledWrites; };
                require(stalled.pollPreparedStart(stalledLifecycle,stalledMask,stalledRead,stalledWrite)
                    == sc55::PreparedVoiceBatch::Status::waitingForReuse);
                require(stalledReads == 6 && stalledWrites == 1 && !stalled.voices[firstSlot]);
                stalledLifecycle[firstSlot].fieldCAF4 = 2;
                require(stalled.pollPreparedStart(stalledLifecycle,stalledMask,stalledRead,stalledWrite)
                    == sc55::PreparedVoiceBatch::Status::cancelled);
                require(stalled.failed() && !stalled.voices[firstSlot] && !stalled.voices[secondSlot]);
                require(stalled.pollPreparedStart(stalledLifecycle,stalledMask,forbiddenRead,forbiddenWrite)
                    == sc55::PreparedVoiceBatch::Status::cancelled);
                require(!stalled.beginPreparedStart(startSlots,*prepared,stalledLifecycle,stalledMask));
                deferredClock.advance(uint64_t(period)*256); // Failed owners must retain this wrapped event.
                require(stalled.serviceControl(deferredClock,pairInstallation,partControllers,pairNotes.allocator,
                    data,conversion,waves,forbiddenRead,forbiddenWrite).status == Scheduled::failed);
                observedClock = deferredClock; require(observedClock.consume() == 0);
                require(invalidIo == 0);
                auto status = engine.pollStart(read,write);
                for (unsigned poll = 0; (status == sc55::PreparedVoiceBatch::Status::waitingForReuse
                    || status == sc55::PreparedVoiceBatch::Status::waitingForKeyLatch) && poll < 1024; ++poll)
                {
                    PCM_Update(*pcm,pcm->cycles+2500); clock.advance(2500);
                    if(status==sc55::PreparedVoiceBatch::Status::waitingForKeyLatch) {
                        auto beforeClock=clock;
                        require(paired.serviceControl(clock,pairInstallation,partControllers,pairNotes.allocator,
                            data,conversion,waves,forbiddenRead,forbiddenWrite).status == Scheduled::deferred);
                        auto afterClock=clock;require(afterClock.consume()==beforeClock.consume());
                    } else {
                        auto beforeClock=clock;const auto elapsed=beforeClock.consume();
                        const auto control=paired.serviceControl(clock,pairInstallation,partControllers,pairNotes.allocator,
                            data,conversion,waves,read,write);
                        require(control.status==(elapsed ? Scheduled::updated : Scheduled::idle));
                        require(!(control.updatedMask&((1u<<firstSlot)|(1u<<secondSlot))));
                    }
                    status = engine.pollStart(read,write);
                }
                require(status == sc55::PreparedVoiceBatch::Status::complete);
                require(!paired.startupPending() && !paired.failed());
                unsigned deliveredPedals = 0;
                for (unsigned i = 0; i < 2; ++i)
                    require(paired.serviceMidi(pendingMidi,pcm->cycles,[&](const auto& event) {
                        require(event.first == 64 && event.second == (deliveredPedals == 0 ? 127 : 0));
                        ++deliveredPedals;
                        require(engine.receiveReleaseMidi(event,routing,{}).status
                            == sc55::NativeVoiceEngine::ReleaseMidiResult::Status::accepted);
                        return sc55::MidiDispatchResult::accepted;
                    }) == sc55::MidiDispatchResult::accepted);
                require(deliveredPedals == 2 && pendingMidi.size() == 0);
                require(paired.pollPreparedStart(lifecycle,mask,forbiddenRead,forbiddenWrite) == status);
                require(invalidIo == 0);
                observedClock = clock;
                const auto expectedElapsed = observedClock.consume();
                const auto resumed = engine.serviceControl(partControllers,data,conversion,waves,read,write);
                require(resumed.status == (expectedElapsed ? Scheduled::updated : Scheduled::idle));
                if (expectedElapsed) require(resumed.elapsed == *expectedElapsed);
                require(paired.serviceControl(clock,pairInstallation,partControllers,pairNotes.allocator,
                    data,conversion,waves,forbiddenRead,forbiddenWrite).status == Scheduled::idle);
                // A complete event whose byte count wrapped to zero must run
                // once, not disappear through a boolean elapsed-value check.
                sc55::VoiceControlRuntime emptyRuntime;
                auto emptyAllocator = pairNotes.allocator;
                const auto wrapped = emptyRuntime.serviceControl(deferredClock,pairInstallation,partControllers,
                    emptyAllocator,data,conversion,waves,forbiddenRead,forbiddenWrite);
                require(wrapped.status == Scheduled::updated && wrapped.elapsed == 0 && wrapped.updatedMask == 0);
                require(invalidIo == 0);
            }
            require(pairedAllocated == 0xffffff && pairNotes.allocator.freeCount == 0
                && deliveredStarts == 12 && startMidi.size() == 0);
            if (combinedStart && pairProgram == pairPrograms.front())
            {
                using Release = sc55::NativeVoiceEngine::ReleaseMidiResult::Status;
                const sc55::MidiDecoder::Event off{sc55::MidiDecoder::Kind::message,0x80,60,0,2};
                auto missing = engine;
                const auto group = missing.notes.allocator.partHead[0]; require(group < 24);
                const auto slot = missing.notes.allocator.groups.head[group]; require(slot < 24);
                missing.runtime.voices[slot].reset();
                const auto before = missing.notes;
                require(missing.receiveReleaseMidi(off,routing,{}).status == Release::failed);
                require(std::memcmp(&missing.notes.allocator,&before.allocator,sizeof(before.allocator)) == 0);
                for (unsigned part = 0; part < 16; ++part)
                    require(missing.notes.part(part)->retainedKeys == before.part(part)->retainedKeys
                        && missing.notes.part(part)->sostenutoEnabled == before.part(part)->sostenutoEnabled);
                for (unsigned i = 0; i < 24; ++i)
                    if (missing.runtime.voices[i]) require(missing.runtime.voices[i]->release.pending == paired.voices[i]->release.pending);
                auto normalOff = engine, zeroVelocity = engine;
                const auto normal = normalOff.receiveReleaseMidi(off,routing,{});
                auto zero = off; zero.status = 0x90;
                const auto alternate = zeroVelocity.receiveReleaseMidi(zero,routing,{});
                require(normal.status == Release::accepted && alternate.status == Release::accepted
                    && normal.matchedParts == 1 && alternate.matchedParts == normal.matchedParts);
                require(std::memcmp(&normalOff.notes.allocator,&zeroVelocity.notes.allocator,sizeof(before.allocator)) == 0);
                for (unsigned i = 0; i < 24; ++i)
                    require(normalOff.runtime.voices[i]->release.pending == zeroVelocity.runtime.voices[i]->release.pending);
                auto invalid = engine;
                require(invalid.receiveReleaseMidi({sc55::MidiDecoder::Kind::message,0xb0,7,100,2},routing,{}).status == Release::invalidInput);
                require(std::memcmp(&invalid.notes.allocator,&engine.notes.allocator,sizeof(before.allocator)) == 0);
                for (uint8_t controller : {uint8_t(64),uint8_t(66)})
                {
                    auto pedalEngine = engine; auto reference = engine;
                    const std::array<sc55::MidiDecoder::Event,3> sequence{
                        sc55::MidiDecoder::Event{sc55::MidiDecoder::Kind::message,0xb0,controller,127,2},off,
                        sc55::MidiDecoder::Event{sc55::MidiDecoder::Kind::message,0xb0,controller,0,2}};
                    for (const auto& event : sequence)
                    {
                        if (event.status == 0x80) require(bool(reference.notes.receiveNoteOff(event,routing,{})));
                        else require(reference.notes.receivePedal(event,routing));
                        require(reference.runtime.publishNoteReleases(reference.notes.allocator));
                        require(pedalEngine.receiveReleaseMidi(event,routing,{}).status == Release::accepted);
                        require(std::memcmp(&pedalEngine.notes.allocator,&reference.notes.allocator,sizeof(before.allocator)) == 0);
                        for (unsigned part = 0; part < 16; ++part)
                            require(pedalEngine.notes.part(part)->retainedKeys == reference.notes.part(part)->retainedKeys
                                && pedalEngine.notes.part(part)->sostenutoEnabled == reference.notes.part(part)->sostenutoEnabled);
                        for (unsigned i = 0; i < 24; ++i)
                            require(pedalEngine.runtime.voices[i]->release.pending == reference.runtime.voices[i]->release.pending);
                    }
                }
            }
            if (combinedStart && pairProgram == pairPrograms.front())
            {
                // Choke uses live DSP state, not the stale installation copy.
                auto choke = engine;
                auto& allocation = choke.notes.allocator;
                const auto part = allocation.allocations[0].part;
                for (unsigned group = 0; group < 24; ++group)
                    allocation.noteGroups[group].noteClass = 60;
                for (auto& state : choke.lifecycle) state.progress ^= 0xffff;
                unsigned io = 0;
                const auto load = [&](uint8_t) { ++io; return uint8_t(0); };
                const auto store = [&](uint8_t,uint8_t) { ++io; };
                using Stop = sc55::NativeVoiceEngine::StopRequest;
                auto missing = choke;
                missing.runtime.voices[0].reset();
                require(missing.requestRhythmChoke(part,60,load,store) == Stop::failed && io == 0);
                auto pending = choke;
                pending.lifecycle[0].fieldCAF4 = 4;
                require(pending.requestRhythmChoke(part,60,load,store) == Stop::deferred && io == 0);
                require(choke.requestRhythmChoke(part,0,load,store) == Stop::queued && io == 0);
                require(choke.requestRhythmChoke(part,60,load,store) == Stop::queued && io > 0);
                require(allocation.partVoiceCount[part] == 0);
                for (unsigned slot = 0; slot < 24; ++slot)
                    if (choke.lifecycle[slot].fieldCAF4 == 4)
                        require(choke.lifecycle[slot].progress == engine.runtime.voices[slot]->lifecycle.progress);
                unsigned consumed = 0;
                while (choke.serviceStopTask().status == sc55::VoiceControlRuntime::StopTaskStatus::completed)
                    require(++consumed <= 24);
                require(consumed > 0);
            }
            outputRange = {}; uint32_t pairVoiced = 0; unsigned pairPasses = 0, pairPeak = 0;
            for (; pairPasses < 8192; ++pairPasses)
            {
                if (pairPasses == 144)
                {
                    sc55::MidiEventQueue<12> noteOffQueue;
                    for (uint8_t note = 60; note < 72; ++note)
                    {
                        const std::array<uint8_t,3> noteOff{0x80,note,0};
                        require(noteOffQueue.push(noteOff,pcm->cycles).consumed == noteOff.size());
                    }
                    for (unsigned i = 0; i < 12; ++i)
                        require(paired.serviceMidi(noteOffQueue,pcm->cycles,[&](const auto& event) {
                            require(event.first == 60+i);
                            auto expectedNotes = pairNotes;
                            const auto expected = expectedNotes.receiveNoteOff(event,routing,{});
                            const auto received = engine.receiveReleaseMidi(event,routing,{});
                            require(received.status == sc55::NativeVoiceEngine::ReleaseMidiResult::Status::accepted
                                && expected && received.matchedParts == *expected);
                            return sc55::MidiDispatchResult::accepted;
                        }) == sc55::MidiDispatchResult::accepted);
                    require(noteOffQueue.size() == 0);
                    if (combinedStart && pairProgram == pairPrograms.front())
                    {
                        // Exercise explicit stop-task delivery against the
                        // running PCM, not just natural Note Off termination.
                        for (unsigned slot = 0; slot < 24; ++slot)
                        {
                            const auto progress = paired.voices[slot]->lifecycle.progress;
                            const auto pcm10 = paired.voices[slot]->lifecycle.pcm10;
                            lifecycle[slot].progress ^= 0xffff; lifecycle[slot].pcm10 ^= 0xffff;
                            require(engine.requestStop(slot,read,write) == sc55::NativeVoiceEngine::StopRequest::queued);
                            require(lifecycle[slot].progress == progress && lifecycle[slot].pcm10 == pcm10);
                        }
                        unsigned prematureIo = 0;
                        const auto noRead = [&](uint8_t) { ++prematureIo; return uint8_t(0); };
                        const auto noWrite = [&](uint8_t,uint8_t) { ++prematureIo; };
                        require(engine.requestStop(0,noRead,noWrite) == sc55::NativeVoiceEngine::StopRequest::deferred);
                        auto pendingEngine = engine;
                        pendingEngine.clock.advance(pendingEngine.clock.untilNextExpiration());
                        const auto stoppedPass=pendingEngine.serviceControl(partControllers,data,conversion,waves,noRead,noWrite);
                        require(stoppedPass.status==sc55::VoiceControlRuntime::ScheduledStatus::updated
                            && stoppedPass.elapsed==1 && stoppedPass.updatedMask==0);
                        require(!pendingEngine.clock.consume() && prematureIo==0);
                        for(const auto& stopped:pendingEngine.lifecycle) require(stopped.fieldCAF4==4);
                        for (unsigned remaining = 24; remaining > 0; --remaining)
                        {
                            const auto stop = engine.serviceStopTask();
                            require(stop.status == sc55::VoiceControlRuntime::StopTaskStatus::completed
                                && stop.slot == remaining-1);
                        }
                        // Task consumption alone must not pretend the PCM
                        // levels are zero or prematurely return these slots.
                        require(pairNotes.allocator.freeCount == 0);
                    }
                }
                const auto duration = clock.untilNextExpiration();
                PCM_Update(*pcm,pcm->cycles+duration); clock.advance(duration);
                const auto scheduled = engine.serviceControl(partControllers,data,conversion,waves,read,write);
                require(scheduled.status == sc55::VoiceControlRuntime::ScheduledStatus::updated && scheduled.elapsed == 1);
                for (unsigned slot = 0; slot < 24; ++slot)
                    require(lifecycle[slot].stages == paired.voices[slot]->lifecycle.stages
                        && lifecycle[slot].progress == paired.voices[slot]->lifecycle.progress
                        && lifecycle[slot].pcm10 == paired.voices[slot]->lifecycle.pcm10);
                unsigned sounding = 0;
                for (unsigned slot = 0; slot < 24; ++slot)
                    if (paired.lastWrites[slot] == sc55::VoicePcmUpdateResult::written && pcm->ram2[slot][9] && pcm->ram2[slot][10])
                    { pairVoiced |= 1u<<slot; ++sounding; }
                pairPeak = std::max(pairPeak,sounding);
                if (pairPasses > 144 && pairNotes.allocator.freeCount == 24) break;
            }
            require(pairVoiced == 0xffffff && pairPeak == 24 && pairNotes.allocator.freeCount == 24);
            if (waveDirectory) require(int64_t(outputRange.maximum)-outputRange.minimum > 65536);
            else require(outputRange.minimum == 0 && outputRange.maximum == 0);
            require(mcu->pc == 0 && mcu->cycles == 0);
            std::printf("Native paired patch startup: program%u, combined%u, 12 complete two-partial notes, all24 voices sounded and returned in %u passes\n",
                pairProgram,unsigned(combinedStart),pairPasses+1);
            concurrent = paired;
            concurrentNotes.allocator = pairNotes.allocator;
        }
        std::printf("Native paired program sweep: %zu capital programs completed through both entry paths\n",pairPrograms.size());
        {
            using Fanout = sc55::NoteOnFanout;
            std::array<sc55::PartMidiReceive,16> fanRouting;
            for (unsigned part = 0; part < 12; ++part) fanRouting[part] = {0,0x6ea2,0x80};
            sc55::ChannelControls channels;
            require(channels.apply({sc55::MidiDecoder::Kind::message,0xc0,uint8_t(pairPrograms.front()),0,1}));
            const sc55::MidiDecoder::Event on{sc55::MidiDecoder::Kind::message,0x90,60,100,2};
            require(engine.beginNoteOn(on,fanRouting,{}));
            sc55::MidiEventQueue<2> following;
            const std::array<uint8_t,3> off{0x80,60,0};
            require(following.push(off,pcm->cycles).consumed == off.size());
            unsigned visits = 0, earlyMessages = 0;
            std::optional<sc55::VoiceControlRuntime::MelodicStartResult> lastMonoStart;
            for (unsigned remaining = 12; remaining > 0; --remaining)
            {
                require(engine.serviceMidi(following,pcm->cycles,[&](auto) {
                    ++earlyMessages; return sc55::MidiDispatchResult::accepted;
                }) == sc55::MidiDispatchResult::deferred);
                const auto fan = engine.serviceNoteOn([&](uint8_t part,const auto& event) {
                    require(part == remaining-1); ++visits;
                    const sc55::PartialSampleInstallInputs sample{scale,60,60,60,0,0,60,0,160};
                    const sc55::NormalPartialDspInputs dsp{60,false,0,preparationRequests[0]->controls,{},{}};
                    const auto started = engine.startRoutedMelodicNote(event,channels.channel(0),
                        {0,false,0,part,0x80,1},{sample,sample},{dsp,dsp},partControllers,data,conversion,waves,read,write);
                    require(started.status == sc55::VoiceControlRuntime::MelodicStartResult::Status::started);
                    if (part==0) lastMonoStart=started;
                    return Fanout::Visit::accepted;
                });
                require(fan == (remaining == 1 ? Fanout::Status::complete : Fanout::Status::ready));
                require(engine.serviceNoteOn([&](auto,const auto&) {
                    ++visits; return Fanout::Visit::accepted;
                }) == Fanout::Status::deferred);
                auto status = engine.pollStart(read,write);
                for (unsigned poll = 0; engine.runtime.startupPending() && poll < 1024; ++poll)
                {
                    PCM_Update(*pcm,pcm->cycles+2500); engine.clock.advance(2500);
                    status = engine.pollStart(read,write);
                }
                require(status == sc55::VoiceControlRuntime::StartStatus::complete);
                if (remaining == 12)
                {
                    // One part has already completed real PCM activation.
                    // Fail the next part on a copied owner; all engine entry
                    // points must now refuse work, not replay that first part.
                    auto failed = engine; auto retainedMidi = following;
                    const auto beforeAllocator = failed.notes.allocator;
                    const auto beforeMask = failed.mask;
                    const auto remainingParts = failed.noteOn.remainingParts();
                    require(failed.serviceNoteOn([&](uint8_t part,const auto&) {
                        require(part == 10); return Fanout::Visit::failed;
                    }) == Fanout::Status::failed);
                    require(failed.failed() && !failed.runtime.failed()
                        && failed.noteOn.remainingParts() == remainingParts);
                    unsigned io = 0, callbacks = 0;
                    const auto noRead = [&](uint8_t) { ++io; return uint8_t(0); };
                    const auto noWrite = [&](uint8_t,uint8_t) { ++io; };
                    failed.clock.advance(failed.clock.untilNextExpiration());
                    auto retainedClock = failed.clock;
                    require(!failed.beginNoteOn(on,fanRouting,{}));
                    require(failed.serviceNoteOn([&](auto,const auto&) { ++callbacks; return Fanout::Visit::accepted; }) == Fanout::Status::failed);
                    require(failed.serviceMidi(retainedMidi,pcm->cycles,[&](auto) {
                        ++callbacks; return sc55::MidiDispatchResult::accepted;
                    }) == sc55::MidiDispatchResult::failed);
                    require(failed.receiveReleaseMidi({sc55::MidiDecoder::Kind::message,0x80,60,0,2},fanRouting,{}).status
                        == sc55::NativeVoiceEngine::ReleaseMidiResult::Status::failed);
                    require(failed.startRoutedMelodicNote(on,channels.channel(0),{0,false,0,0,0x80,1},{},{},
                        partControllers,data,conversion,waves,noRead,noWrite).status
                        == sc55::VoiceControlRuntime::MelodicStartResult::Status::failed);
                    require(failed.requestStop(0,noRead,noWrite) == sc55::NativeVoiceEngine::StopRequest::failed);
                    require(failed.serviceStopTask().status == sc55::VoiceControlRuntime::StopTaskStatus::failed);
                    require(failed.pollStart(noRead,noWrite) == sc55::VoiceControlRuntime::StartStatus::cancelled);
                    require(failed.serviceControl(partControllers,data,conversion,waves,noRead,noWrite).status
                        == sc55::VoiceControlRuntime::ScheduledStatus::failed);
                    require(failed.clock.consume() == retainedClock.consume());
                    require(io == 0 && callbacks == 0 && retainedMidi.size() == following.size()
                        && failed.mask.enabled == beforeMask.enabled && failed.mask.prepared == beforeMask.prepared);
                    require(std::memcmp(&failed.notes.allocator,&beforeAllocator,sizeof(beforeAllocator)) == 0);
                }
                const auto control = engine.serviceControl(partControllers,data,conversion,waves,read,write);
                require(control.status == sc55::VoiceControlRuntime::ScheduledStatus::idle
                    || control.status == sc55::VoiceControlRuntime::ScheduledStatus::updated);
            }
            require(visits == 12 && earlyMessages == 0 && following.size() == 1 && engine.notes.allocator.freeCount == 0);
            {
                require(bool(lastMonoStart));
                auto& allocator=engine.notes.allocator;
                const auto oldGroup=allocator.partHead[0];
                const auto oldHead=allocator.groups.head[oldGroup],oldTail=allocator.groups.tail[oldGroup];
                const auto tone=engine.installation.voices[oldTail].input.tone;
                const auto selected=sc55::PrepareMappedNoteVelocity(
                    {sc55::MidiDecoder::Kind::message,0x90,67,110,2},tone,false,0,data);
                require(bool(selected));
                const sc55::PartialSampleInstallInputs sample{scale,67,67,67,255,0,67,0,0x5f};
                std::array<sc55::NormalPartialDspInputs,2> dsp{};
                for (unsigned i=0;i<lastMonoStart->requests->count;++i) {
                    const auto& entry=lastMonoStart->requests->entries[i];
                    const auto partial=entry.request.installed.input.partial;
                    dsp[partial]={60,false,0,engine.runtime.inputs[entry.slot],{},
                        lastMonoStart->prepared->voices[i]->partPitch};
                }
                const auto reused=engine.startReusedMelodicNote(*selected,0,{sample,sample},dsp,
                    partControllers,data,conversion,waves,read,write);
                require(reused.status==sc55::VoiceControlRuntime::MelodicStartResult::Status::started);
                require(allocator.freeCount==0 && allocator.partHead[0]==oldGroup
                    && allocator.groups.head[oldGroup]==oldHead && allocator.groups.tail[oldGroup]==oldTail);
                for (unsigned poll=0;engine.runtime.startupPending() && poll<1024;++poll) {
                    PCM_Update(*pcm,pcm->cycles+2500); engine.clock.advance(2500);
                    engine.pollStart(read,write);
                }
                require(!engine.failed() && !engine.runtime.startupPending());
                require(engine.installation.voices[oldHead].input.originalKey==67
                    && engine.installation.voices[oldTail].input.originalKey==67);
            }
            outputRange = {}; uint32_t sounded = 0;
            for (unsigned tick = 0; tick < 8192; ++tick)
            {
                if (tick == 144)
                    require(engine.serviceMidi(following,pcm->cycles,[&](const auto& event) {
                        const auto release = engine.receiveReleaseMidi(event,fanRouting,{});
                        require(release.status == sc55::NativeVoiceEngine::ReleaseMidiResult::Status::accepted);
                        return sc55::MidiDispatchResult::accepted;
                    }) == sc55::MidiDispatchResult::accepted);
                const auto duration = engine.clock.untilNextExpiration();
                PCM_Update(*pcm,pcm->cycles+duration); engine.clock.advance(duration);
                require(engine.serviceControl(partControllers,data,conversion,waves,read,write).status
                    == sc55::VoiceControlRuntime::ScheduledStatus::updated);
                for (unsigned slot = 0; slot < 24; ++slot)
                    if (pcm->ram2[slot][9] && pcm->ram2[slot][10]) sounded |= 1u<<slot;
                if (tick > 144 && engine.notes.allocator.freeCount == 24) break;
            }
            require(sounded == 0xffffff && following.size() == 0 && engine.notes.allocator.freeCount == 24);
            if (waveDirectory) require(int64_t(outputRange.maximum)-outputRange.minimum > 65536);
            require(mcu->pc == 0 && mcu->cycles == 0);
            std::puts("Native Note On fan-out: one event ->12 parts ->24 voices sounded and reclaimed; no H8 execution");
        }
        if (data.patchCount() > 385)
        for (unsigned rhythmTone = 224; rhythmTone < 386; ++rhythmTone)
        {
            // Isolate the drum waveform from any preceding voices/effect tails.
            pcm = std::make_unique<pcm_t>(); PCM_Init(*pcm,*mcu); PCM_UseSimulation(*pcm,false);
            if (waveDirectory)
            {
                std::copy(waveData[0].begin(),waveData[0].end(),pcm->waverom1);
                std::copy(waveData[1].begin(),waveData[1].end(),pcm->waverom2);
                std::copy(waveData[2].begin(),waveData[2].end(),pcm->waverom3);
            }
            write(0x3c,0); write(0x3d,0xb7);
            // Explicit research configuration, not a native GS reset image.
            sc55::NativeVoiceEngine drum;
            require(drum.notes.allocator.initializeTables());
            for (unsigned slot = 0; slot < 24; ++slot)
                drum.runtime.first[slot].firstStage = drum.runtime.second[slot].firstStage = 22;
            sc55::RhythmKeyMap map; map.tones[46] = uint16_t(rhythmTone); map.pitches[46] = 60;
            map.groups[46] = 1; map.flags[46] = 0x90;
            std::array<uint8_t,128> accumulators{};
            const sc55::MidiDecoder::Event event{sc55::MidiDecoder::Kind::message,0x99,46,100,2};
            const auto selection = sc55::PrepareRhythmNoteVelocity(event,0,map,accumulators,0,false,data);
            require(selection && selection->note);
            const sc55::NormalPartialDspInputs dsp{60,false,0,preparationRequests[0]->controls,{},{}};
            const auto started = drum.startRoutedRhythmNote(*selection,9,0x10,map,64,0,scale,
                {0,0},{160,160},{},{dsp,dsp},partControllers,data,conversion,waves,read,write);
            require(started.status == sc55::NativeVoiceEngine::RhythmStartResult::Status::started);
            auto allocation = *started.allocation;
            const auto waitingLife = drum.lifecycle;
            const auto waitingAllocator = drum.notes.allocator;
            require(drum.startRoutedRhythmNote(*selection,9,0x10,map,64,0,scale,
                {0,0},{160,160},{},{dsp,dsp},partControllers,data,conversion,waves,
                [](uint8_t)->uint8_t { throw std::runtime_error("Busy drum startup read"); },
                [](uint8_t,uint8_t) { throw std::runtime_error("Busy drum startup write"); }).status
                == sc55::NativeVoiceEngine::RhythmStartResult::Status::deferred);
            require(std::memcmp(&waitingLife,&drum.lifecycle,sizeof waitingLife) == 0);
            require(std::memcmp(&waitingAllocator,&drum.notes.allocator,sizeof waitingAllocator) == 0);
            unsigned polls = 0;
            while (drum.runtime.startupPending())
            {
                require(++polls < 1024);
                PCM_Update(*pcm,pcm->cycles+2500); drum.clock.advance(2500);
                require(drum.pollStart(read,write) != sc55::VoiceControlRuntime::StartStatus::cancelled);
            }
            if (rhythmTone == 224)
            {
                // A second event performs its own choke exactly once. The old
                // task4 must not make the new task2 fail dispatcher matching.
                const auto retrigger = drum.startRoutedRhythmNote(*selection,9,0x10,map,64,0,scale,
                    {0,0},{160,160},{},{dsp,dsp},partControllers,data,conversion,waves,read,write);
                require(retrigger.status == sc55::NativeVoiceEngine::RhythmStartResult::Status::started);
                allocation = *retrigger.allocation;
                require(drum.notes.allocator.partVoiceCount[9] == selection->note->partials.candidates.count);
                polls = 0;
                while (drum.runtime.startupPending())
                {
                    require(++polls < 1024);
                    PCM_Update(*pcm,pcm->cycles+2500); drum.clock.advance(2500);
                    require(drum.pollStart(read,write) != sc55::VoiceControlRuntime::StartStatus::cancelled);
                }
            }
            outputRange = {}; bool voiced = false;
            for (unsigned tick = 0; tick < 512; ++tick)
            {
                const auto duration = drum.clock.untilNextExpiration();
                PCM_Update(*pcm,pcm->cycles+duration); drum.clock.advance(duration);
                require(drum.serviceControl(partControllers,data,conversion,waves,read,write).status
                    == sc55::VoiceControlRuntime::ScheduledStatus::updated);
                for (const auto slot : allocation.group->voices)
                    if (slot < 24 && pcm->ram2[slot][9] && pcm->ram2[slot][10]) voiced = true;
            }
            require(voiced && outputRange.frames > 0 && mcu->pc == 0 && mcu->cycles == 0);
            if (waveDirectory) require(int64_t(outputRange.maximum)-outputRange.minimum > 65536);
            else require(outputRange.minimum == 0 && outputRange.maximum == 0);
            require(drum.requestRhythmChoke(9,1,read,write) == sc55::NativeVoiceEngine::StopRequest::queued);
            require(drum.notes.allocator.freeCount == 24);
            unsigned stopTasks = 0;
            while (true)
            {
                const auto task = drum.serviceStopTask();
                if (task.status == sc55::VoiceControlRuntime::StopTaskStatus::idle) break;
                require(task.status == sc55::VoiceControlRuntime::StopTaskStatus::completed && ++stopTasks <= 2);
            }
            // Some one-shot samples may already have finished naturally.
            bool reusable = false;
            for (unsigned tick = 0; tick < 512 && !reusable; ++tick)
            {
                const auto duration = drum.clock.untilNextExpiration();
                PCM_Update(*pcm,pcm->cycles+duration); drum.clock.advance(duration);
                require(drum.serviceControl(partControllers,data,conversion,waves,read,write).status
                    == sc55::VoiceControlRuntime::ScheduledStatus::updated);
                reusable = true;
                for (const auto slot : allocation.group->voices)
                    if (slot < 24)
                        reusable &= sc55::PollVoiceReuse(slot,drum.lifecycle[slot].fieldCAF4,read,write)
                            == sc55::VoiceReuseReadiness::ready;
            }
            require(reusable && !drum.failed());
            std::printf("Native bank2 tone %u: sounded, choked/reclaimed and ready for reuse without H8 execution\n",rhythmTone);
        }
    }
    return 0;
}
