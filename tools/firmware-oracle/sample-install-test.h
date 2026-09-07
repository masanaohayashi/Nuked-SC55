#pragma once
#include "sc55_sample_install.h"
#include "sc55_voice_prepare.h"
#include <vector>
#include <tuple>
#include <stdexcept>
#include <cstring>
#include <type_traits>

inline void verifySampleInstallation(const sc55::SoundData& data)
{
    const auto require = [](bool ok) { if (!ok) throw std::runtime_error("Sample installation regression"); };
    static_assert(std::has_unique_object_representations_v<sc55::VoiceAllocator>);
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
            if (flags == 0)
            {
                require(!taskInputs); // Non-restarted voices require the re-entry path.
                for (const auto& sample : *result)
                    if (sample && sample->installed) require(actualLife[sample->slot].fieldCAF4 == 2);
            }
            else
            {
                const auto expectedTask = sc55::DispatchNextVoiceTask(expectedLife,expected.pcmLinks,expected.activity);
                require(taskInputs && expectedTask && taskInputs->count == expectedTask->count);
                for (unsigned i = 0; i < taskInputs->count; ++i)
                {
                    const auto& entry = taskInputs->entries[i]; const auto partial = entry.request.installed.input.partial;
                    require(entry.slot == expectedTask->slots[i] && entry.lifecycle.fieldCAF4 == 0);
                    require(entry.request.sourceKey == 61+partial && entry.request.unoffsetStart == (partial != 0)
                        && entry.request.historyNibble == 5+partial && entry.previousPitch.cachedRandom == 71+partial
                        && entry.request.controls.amplitude.attack == 3+partial);
                }
            }
        }
}
