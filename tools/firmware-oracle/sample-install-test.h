#pragma once
#include "sc55_sample_install.h"
#include "sc55_voice_prepare.h"
#include "sc55_voice_runtime.h"
#include <vector>
#include <tuple>
#include <stdexcept>
#include <cstring>
#include <type_traits>

inline void verifySampleInstallation(const sc55::SoundData& data)
{
    const auto require = [](bool ok) { if (!ok) throw std::runtime_error("Sample installation regression"); };
    static_assert(std::has_unique_object_representations_v<sc55::VoiceAllocator>);
    {
        // Explicit one-slot/two-partial fixture: installation must execute
        // twice, while the later task2 owns only the final partial metadata.
        sc55::VoiceAllocator allocator; require(allocator.initializeTables());
        const auto group=allocator.createGroup({0,0x80,60,1,1}); require(bool(group));
        const auto tone=sc55::ResolveV121MelodicPreset(0,80); require(bool(tone));
        const auto selection=sc55::PrepareMappedNoteVelocity(
            {sc55::MidiDecoder::Kind::message,0x90,60,100,2},*tone,false,0,data);
        require(selection && selection->partials.candidates.count==2);
        const auto allocation=sc55::PrepareMonoReuseAllocation(*selection,0,data,allocator);
        require(allocation.status==sc55::MelodicAllocationResult::Status::allocated
            && allocation.dispatch[0].voice==allocation.dispatch[1].voice);
        sc55::VoiceInstallationState installation;
        std::array<sc55::VoiceStopState,24> life{};
        sc55::PartialSampleInstallInputs input{}; input.scale.fill(64);
        input.initialKey=input.sourceKey=input.originalNote=input.remappedNote=60;
        input.minimumKey=255; input.flags=0xff;
        auto secondInput=input;
        secondInput.initialKey=secondInput.sourceKey=secondInput.originalNote=secondInput.remappedNote=72;
        auto steppedAllocator=allocator;
        auto steppedInstallation=installation;
        auto steppedLife=life;
        auto plan=sc55::MelodicSampleInstallation::prepare(allocation,0,{input,secondInput},data,
            steppedAllocator,steppedInstallation);
        require(plan.has_value() && !plan->result());
        require(std::memcmp(&allocator,&steppedAllocator,sizeof allocator)==0);
        std::vector<std::pair<uint8_t,uint8_t>> immediateWrites,steppedWrites;
        const auto samples=sc55::PrepareAndInstallMelodicSamples(allocation,0,{input,secondInput},data,
            allocator,installation,life,[](uint8_t) { return uint8_t(0); },
            [&](uint8_t a,uint8_t v) {immediateWrites.emplace_back(a,v);});
        const auto resume=[&] {return plan->resume(steppedAllocator,steppedInstallation,steppedLife,
            [](uint8_t) {return uint8_t(0);},[&](uint8_t a,uint8_t v) {steppedWrites.emplace_back(a,v);});};
        using Progress=sc55::MelodicSampleInstallation::Progress;
        require(resume()==Progress::advanced && !plan->result());
        require(steppedInstallation.voices[group->voices[0]].input.partial==0);
        require(resume()==Progress::complete && plan->result());
        require(steppedInstallation.voices[group->voices[0]].input.partial==1);
        require(resume()==Progress::complete && steppedWrites==immediateWrites);
        require(std::memcmp(&allocator,&steppedAllocator,sizeof allocator)==0);
        require(samples && (*samples)[0] && (*samples)[1]
            && (*samples)[0]->installed && (*samples)[1]->installed);
        require(installation.voices[group->voices[0]].input.partial==1);
        std::array<uint8_t,2> history{77,88};
        sc55::CapturePartialPitchHistory(*samples).apply(history,60);
        require(history==std::array<uint8_t,2>{60,72});
        auto noDestination=*samples;
        noDestination[0]->slot=255; noDestination[0]->installed.reset();
        history={77,88}; sc55::CapturePartialPitchHistory(noDestination).apply(history,60);
        require(history==std::array<uint8_t,2>{60,72});
        noDestination[0].reset(); noDestination[1]->sample.key.storedAdjustedKey.reset();
        history={77,88}; sc55::CapturePartialPitchHistory(noDestination).apply(history,91);
        require(history==std::array<uint8_t,2>{77,91});
        std::array<sc55::NormalPartialDspInputs,2> dsp{};
        dsp[0].sourceKey=48; dsp[1].sourceKey=72;
        const auto dispatched=sc55::DispatchNormalVoiceInputs(*selection,*samples,dsp,life,
            allocator.pcmLinks,allocator.activity);
        require(dispatched && dispatched->count==1 && dispatched->entries[0].slot==group->voices[0]
            && dispatched->entries[0].request.installed.input.partial==1
            && dispatched->entries[0].request.sourceKey==72 && allocator.freeCount==23);
        sc55::VoiceControlRuntime runtime;
        sc55::VoiceKeyMask mask;
        sc55::PartControllerState controllers;
        sc55::PitchConversion conversion;
        sc55::LfoWaveformTables waves;
        auto unassigned=*samples;
        for(auto& partial:unassigned) { partial->slot=255; partial->installed.reset(); }
        std::array<sc55::VoiceStopState,24> idleLife{};
        unsigned io=0;
        const auto read=[&](uint8_t) { ++io; return uint8_t(0); };
        const auto write=[&](uint8_t,uint8_t) { ++io; };
        const auto only=runtime.beginInstalledNote(*selection,unassigned,dsp,allocator,idleLife,mask,
            controllers,data,conversion,waves,read,write);
        require(only.status==sc55::VoiceControlRuntime::MelodicStartResult::Status::preparedOnly
            && only.requests && only.requests->count==0 && only.prepared && only.prepared->count==0
            && !runtime.failed() && !runtime.startupPending() && mask.prepared==0 && io==0);
        history={77,88}; only.pitchHistory.apply(history,60);
        require(history==std::array<uint8_t,2>{60,72});
        auto invalid=runtime; unassigned[0]->slot=24;
        const auto bad=invalid.beginInstalledNote(*selection,unassigned,dsp,allocator,idleLife,mask,
            controllers,data,conversion,waves,read,write);
        require(bad.status==sc55::VoiceControlRuntime::MelodicStartResult::Status::failed && io==0);
        // Actual restart/negative installation returns the allocator slot;
        // the runtime must retain the old owner with its stopped lifecycle.
        const auto slot=group->voices[0];
        const auto amplitude=sc55::PreparePartialAmplitude(*data.patch(*tone),1,(*samples)[1]->sample,
            *data.samples(),72,selection->partials.partials[1]->amplitude,sc55::EnvelopeStage::attack1,64,
            *data.levels(),*data.keyLevels(),*data.keys(),*data.times());
        require(bool(amplitude));
        runtime.voices[slot]=sc55::VoiceControlState{*amplitude};
        runtime.voices[slot]->lifecycle.stages.fill(4);
        runtime.voices[slot]->release.pending=255;
        {
            auto boundaryVoice=*runtime.voices[slot];
            sc55::VoiceControlInputs boundaryInputs;
            sc55::VoiceLinks boundaryLinks;
            boundaryVoice.alternatePitchReference=10000;
            boundaryVoice.pitch.glide.pitch.accumulator=22000;
            boundaryVoice.pitch.glide.pitch.correction={77,12};
            std::vector<std::pair<uint8_t,uint8_t>> boundaryWrites;
            unsigned boundaryReads=0;
            const auto br=[&](uint8_t) { ++boundaryReads; return uint8_t(0); };
            const auto bw=[&](uint8_t a,uint8_t v) { boundaryWrites.emplace_back(a,v); };
            require(sc55::HandleVoicePcmBoundary(slot,boundaryVoice,boundaryInputs,boundaryLinks,conversion,br,bw));
            require(boundaryInputs.pitchReference==10000 && boundaryVoice.pitch.pcmWord==0x800c
                && boundaryVoice.pitch.glide.pitch.correction.source==0
                && boundaryVoice.pitch.glide.pitch.correction.offset==12 && boundaryReads==0
                && boundaryWrites==std::vector<std::pair<uint8_t,uint8_t>>{{0x3e,slot},{0x10,0x80},{0x11,0x0c}});
            boundaryVoice.stopAtSampleEnd=true;
            boundaryLinks.first[slot]=0; boundaryLinks.second[0]=slot;
            boundaryWrites.clear();
            require(sc55::HandleVoicePcmBoundary(slot,boundaryVoice,boundaryInputs,boundaryLinks,conversion,br,bw));
            require(boundaryVoice.lifecycle.stages==std::array<uint16_t,3>{16,16,16}
                && boundaryVoice.lifecycle.cached18==0xb6 && boundaryLinks.first[slot]==255
                && boundaryLinks.second[0]==255 && boundaryWrites.back()==std::pair<uint8_t,uint8_t>{0x19,0xb6});
            boundaryWrites.clear();
            require(sc55::HandleVoicePcmBoundary(slot,boundaryVoice,boundaryInputs,boundaryLinks,conversion,br,bw)
                && boundaryWrites.empty()); // Repeated IRQ on stopped voice does nothing.
        }
        auto negativeInput=installation.voices[slot].input; negativeInput.sample=0xffff;
        uint8_t negativeFlags=0xff;
        require(sc55::RestartAndInstallVoice(slot,negativeInput,negativeFlags,allocator,installation,
            idleLife[slot],read,write));
        require(allocator.freeCount==24 && (allocator.allocations[slot].status&128));
        auto returned=(*samples)[0]; returned->installed.reset(); returned->sample.sampleId=0xffff;
        sc55::InstalledPartialSamples returnedSamples{returned,{}};
        const auto stopIo=io;
        const auto silent=runtime.beginInstalledNote(*selection,returnedSamples,dsp,allocator,idleLife,mask,
            controllers,data,conversion,waves,read,write);
        require(silent.status==sc55::VoiceControlRuntime::MelodicStartResult::Status::preparedOnly
            && silent.requests->count==0 && runtime.voices[slot]
            && runtime.voices[slot]->lifecycle.stages==idleLife[slot].stages
            && runtime.voices[slot]->lifecycle.cached16==idleLife[slot].cached16
            && runtime.voices[slot]->lifecycle.cached18==idleLife[slot].cached18
            && runtime.voices[slot]->release.pending==0 && io==stopIo && !runtime.startupPending());
        // A positive second installation after a negative first one clears
        // free status again. The historical return must not reject task2's
        // final metadata or manufacture a second DSP owner.
        auto positiveInput=(*samples)[1]->installed->input;
        uint8_t positiveFlags=0xff;
        require(sc55::RestartAndInstallVoice(slot,positiveInput,positiveFlags,allocator,installation,
            idleLife[slot],read,write));
        require(!(allocator.allocations[slot].status&128) && idleLife[slot].fieldCAF4==2);
        auto mixed=returnedSamples;
        mixed[1]=(*samples)[1]; mixed[1]->installed=installation.voices[slot];
        sc55::VoiceControlRuntime mixedRuntime;
        const auto restarted=mixedRuntime.beginInstalledNote(*selection,mixed,dsp,allocator,idleLife,mask,
            controllers,data,conversion,waves,read,write);
        require(restarted.status==sc55::VoiceControlRuntime::MelodicStartResult::Status::started
            && restarted.requests && restarted.requests->count==1
            && restarted.requests->entries[0].request.installed.input.partial==1
            && restarted.prepared && restarted.prepared->count==1 && !mixedRuntime.failed());
    }
    for (unsigned program = 0; program < 128; ++program)
        for (uint8_t flags : {uint8_t(0),uint8_t(160)})
        {
            sc55::VoiceAllocator actual; require(actual.initializeTables());
            sc55::ChannelControls channels;
            require(channels.apply({sc55::MidiDecoder::Kind::message,0xc0,uint8_t(program),0,1}));
            const auto allocation = sc55::AllocateMelodicNote({sc55::MidiDecoder::Kind::message,0x90,60,100,2},
                channels.channel(0),{0,false,0,3,0x80,1},data,actual);
            require(allocation.status == sc55::MelodicAllocationResult::Status::allocated);
            auto expected = actual;
            sc55::VoiceInstallationState actualInstall,expectedInstall;
            actualInstall.pendingRelease.fill(77); expectedInstall = actualInstall;
            std::array<sc55::VoiceStopState,24> actualLife{},expectedLife{};
            for (auto& life : actualLife) { life.stages.fill(10); life.fieldCB30 = 99; life.progress = 42; }
            expectedLife = actualLife;
            sc55::PartialSampleInstallInputs input{}; input.scale.fill(64);
            input.initialKey = input.sourceKey = input.originalNote = input.remappedNote = 60;
            input.flags = flags; input.sampleMode = 7; // Distinct from key-resolution mode0.
            const std::array<sc55::PartialSampleInstallInputs,2> inputs{input,input};
            std::vector<std::pair<uint8_t,uint8_t>> actualWrites,expectedWrites;
            unsigned actualReads = 0,expectedReads = 0;
            const auto actualRead = [&](uint8_t a) { return uint8_t(a+(++actualReads)*7); };
            const auto expectedRead = [&](uint8_t a) { return uint8_t(a+(++expectedReads)*7); };
            const auto actualWrite = [&](uint8_t a,uint8_t v) { actualWrites.emplace_back(a,v); };
            const auto expectedWrite = [&](uint8_t a,uint8_t v) { expectedWrites.emplace_back(a,v); };
            const auto result = sc55::PrepareAndInstallMelodicSamples(allocation,3,inputs,data,
                actual,actualInstall,actualLife,actualRead,actualWrite);
            require(bool(result));
            const auto& selected = *allocation.selection;
            const auto& patch = *data.patch(selected.tone);
            for (unsigned partial = 0; partial < 2; ++partial)
            {
                const auto dispatch = allocation.dispatch[partial];
                if (!dispatch.prepare) { require(!(*result)[partial]); continue; }
                const auto plan = sc55::PreparePartialSample(patch.partial[partial],*data.samples(),60,60,60,input.scale,0,0,60);
                require(plan && (*result)[partial] && (*result)[partial]->sample.sampleId == plan->sampleId);
                const sc55::VoiceInstallationInput request{selected.tone,plan->sampleId,uint8_t(partial),3,
                    plan->key.storedOriginalNote.value_or(60),plan->key.storedAdjustedKey.value_or(60),
                    selected.velocity,7,plan->key.lookupKey,false};
                auto preparationFlags = flags;
                require(sc55::RestartAndInstallVoice(dispatch.voice,request,preparationFlags,
                    expected,expectedInstall,expectedLife[dispatch.voice],expectedRead,expectedWrite));
            }
            require(actualWrites == expectedWrites && actualReads == expectedReads);
            require(std::memcmp(&actual,&expected,sizeof(actual)) == 0);
            require(actualInstall.pendingRelease == expectedInstall.pendingRelease);
            for (unsigned slot = 0; slot < 24; ++slot)
            {
                const auto& a = actualInstall.voices[slot]; const auto& b = expectedInstall.voices[slot];
                require(a.input == b.input && a.flags == b.flags && a.taskState == b.taskState);
                const auto fields = [](const auto& s) { return std::tie(s.stages,s.cached16,s.cached18,
                    s.fieldCB30,s.fieldCAF4,s.savedStage,s.progress,s.delayAccumulator,s.pcm10,s.flagMinus3B,s.fieldC8B3); };
                require(fields(actualLife[slot]) == fields(expectedLife[slot]));
            }
            std::array<sc55::NormalPartialDspInputs,2> dspInputs{};
            for (unsigned partial = 0; partial < 2; ++partial)
            {
                dspInputs[partial].sourceKey = uint8_t(61+partial);
                dspInputs[partial].unoffsetStart = partial != 0;
                dspInputs[partial].historyNibble = uint8_t(5+partial);
                dspInputs[partial].previousPitch.cachedRandom = uint8_t(71+partial);
                dspInputs[partial].controls.amplitude.attack = uint8_t(3+partial);
            }
            const auto taskInputs = sc55::DispatchNormalVoiceInputs(selected,*result,dspInputs,
                actualLife,actual.pcmLinks,actual.activity);
            {
                const auto expectedTask = sc55::DispatchNextVoiceTask(expectedLife,expected.pcmLinks,expected.activity);
                require(taskInputs && expectedTask && taskInputs->count == expectedTask->count);
                for (unsigned i = 0; i < taskInputs->count; ++i)
                {
                    const auto& entry = taskInputs->entries[i]; const auto partial = entry.request.installed.input.partial;
                    require(entry.slot == expectedTask->slots[i] && entry.lifecycle.fieldCAF4 == 0);
                    require(entry.request.installed.flags==flags);
                    require(entry.request.sourceKey == 61+partial && entry.request.unoffsetStart == (partial != 0)
                        && entry.request.historyNibble == 5+partial && entry.previousPitch.cachedRandom == 71+partial
                        && entry.request.controls.amplitude.attack == 3+partial);
                }
            }
        }
}
