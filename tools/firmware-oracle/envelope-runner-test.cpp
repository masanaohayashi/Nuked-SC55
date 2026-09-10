#include "sc55_envelope_runner.h"
#include "sc55_envelope_pcm.h"
#include "sc55_voice_links.h"
#include "sc55_voice_allocator.h"
#include <stdexcept>

int main()
{
    const auto require = [](bool ok) { if (!ok) throw std::runtime_error("envelope runner regression"); };
    using Stage = sc55::EnvelopeStage;
    using Runner = sc55::EnvelopeRunner;
    sc55::VoiceAllocator allocator;
    for (bool prepend : {false,true})
    {
        sc55::VoiceAllocator state;
        require(state.initializeTables());
        const auto group = state.createGroup({0,60,100,0,1});
        require(group.has_value());
        const auto voice = group->voices[0];
        state.shortage = 1; state.activity[voice] = 100;
        // Reclaim deliberately does not skip status 94, unlike normal return.
        require(state.reclaimStoppedVoice(voice,group->group,0,prepend));
        require(state.shortage == 0 && state.activity[voice] == 0 && state.freeCount == 24);
        require(prepend ? state.freeHead == voice : state.freeTail == voice);
        const auto head = state.freeHead;
        require(!state.reclaimStoppedVoice(voice,24,0,prepend) && state.freeHead == head && state.freeCount == 24);
    }
    {
        sc55::VoiceAllocator state;
        require(state.initializeTables());
        using Pass = sc55::VoiceAllocator::CandidatePass;
        std::array<uint8_t,24> activity;
        activity.fill(255);
        require(state.selectCandidate(0,60,Pass::otherValue,activity)->voice == 255);
        require(!state.selectCandidate(16,60,Pass::otherValue,activity));
        state.partHead[0] = 0; state.noteGroups[0].next = 255; state.noteGroups[0].key = 61;
        state.noteGroups[0].status = 0; state.groups.tail[0] = 1; state.groups.previous[1] = 0;
        activity[0] = activity[1] = 10;
        require(state.selectCandidate(0,60,Pass::otherValue,activity)->voice == 1);
        require(state.selectCandidate(0,60,Pass::nonzeroStatusOtherValue,activity)->voice == 255);
        require(state.selectCandidate(0,61,Pass::sameValue,activity)->voice == 1);
        activity[0] = 9;
        require(state.selectCandidate(0,60,Pass::otherValue,activity)->voice == 0);
        state.noteGroups[0].next = 0;
        require(!state.selectCandidate(0,60,Pass::otherValue,activity));
        state.noteGroups[0].next = 255; state.groups.tail[0] = 24;
        require(!state.selectCandidate(0,60,Pass::otherValue,activity));
    }
    {
        sc55::VoiceAllocator boot;
        require(!boot.initializeTables(0,24) && !boot.initializeTables(24,25));
        require(boot.initializeTables() && boot.freeHead == 23 && boot.freeTail == 0 && boot.freeCount == 24);
        std::array<sc55::VoiceAllocator::GroupAllocation,12> allocated;
        for (unsigned i = 0; i < allocated.size(); ++i)
        {
            const auto group = boot.createGroup({uint8_t(i%16),60,100,0,2});
            require(group.has_value()); allocated[i] = *group;
            require(group->group == i && group->voices[1] == 23-i*2 && group->voices[0] == 22-i*2);
            // Activation status is set by a different firmware routine.
            for (unsigned n = 0; n < 2; ++n) boot.allocations[group->voices[n]].status = 0;
        }
        require(boot.freeCount == 0 && !boot.createGroup({0,60,100,0,1}));
        for (const auto& group : allocated)
            for (unsigned n = 0; n < 2; ++n) require(boot.returnVoice(group.voices[n]));
        require(boot.freeCount == 24);
        for (auto count : boot.partVoiceCount) require(count == 0);
        require(boot.createGroup({0,60,100,0,2}).has_value());
    }
    {
        sc55::VoiceAllocator state;
        const sc55::VoiceAllocator::GroupRequest request{0,60,90,17,2};
        require(!state.createGroup(request));
        state.freeGroupHead = 0; state.noteGroups[0].next = 1;
        state.partHead.fill(255); state.partTail.fill(255);
        state.partMinimum.fill(127);
        state.freeHead = 0; state.freeTail = 1; state.freeCount = 2;
        state.allocations[0].nextFree = 255; // missing second voice must roll back first allocation
        require(!state.createGroup(request) && state.freeHead == 0 && state.freeGroupHead == 0 && state.partHead[0] == 255);
        state.allocations[0].nextFree = 0; // repeated free-list slot also rejected
        require(!state.createGroup(request) && state.freeCount == 2);
        state.allocations[0].nextFree = 1; state.allocations[1].nextFree = 255;
        const auto result = state.createGroup(request);
        require(result && result->group == 0 && result->voices[0] == 1 && result->voices[1] == 0
            && result->voices[2] == 255 && state.freeCount == 0 && state.partVoiceCount[0] == 2
            && state.noteGroups[0].noteClass == 60 && state.noteGroups[0].key == 90 && state.noteGroups[0].releaseFlags == 17
            && state.groups.head[0] == 0 && state.groups.tail[0] == 1 && state.partMinimum[0] == 90);
        // Retain native state across creation and both returns. Firmware marks
        // voice status elsewhere; this fixture starts with active status zero.
        require(state.returnVoice(0) && state.partVoiceCount[0] == 1 && state.groups.head[0] == 1);
        require(state.returnVoice(1) && state.partVoiceCount[0] == 0 && state.freeCount == 2
            && state.freeGroupHead == 0 && state.partHead[0] == 255 && state.partMinimum[0] == 127);
        const auto reused = state.createGroup(request);
        require(reused && reused->group == 0 && reused->voices == result->voices);
        require(!state.createGroup({16,0,0,0,1}) && !state.createGroup({0,0,0,0,0}) && !state.createGroup({0,0,0,0,3}));
    }
    require(!allocator.takeFreeVoice());
    allocator.freeHead = 0; allocator.allocations[0].nextFree = 24;
    require(!allocator.takeFreeVoice() && allocator.freeHead == 0);
    allocator.allocations[0].nextFree = 255; allocator.freeCount = 1;
    require(allocator.takeFreeVoice() == 0 && allocator.freeHead == 255 && allocator.freeTail == 255 && allocator.freeCount == 0);
    require(!allocator.attachVoice(24,0,0) && !allocator.attachVoice(0,24,0) && !allocator.attachVoice(0,0,16));
    allocator.pcmLinks.first[0] = 1; allocator.pcmLinks.second[0] = 24;
    allocator.pcmLinks.second[1] = 0;
    require(!allocator.attachVoice(0,0,0) && allocator.pcmLinks.second[1] == 0 && allocator.groups.head[0] == 255);
    allocator.pcmLinks.second[0] = 255;
    require(allocator.attachVoice(0,0,0) && allocator.pcmLinks.second[1] == 255 && allocator.groups.head[0] == 0);
    require(allocator.attachVoice(1,0,0) && allocator.groups.head[0] == 0 && allocator.groups.tail[0] == 1
        && allocator.groups.next[0] == 1 && allocator.groups.previous[1] == 0);
    allocator = sc55::VoiceAllocator{};
    for(auto& voice:allocator.allocations) voice.status=0x94;
    require(!allocator.returnVoice(24) && allocator.returnVoice(0));
    allocator.allocations[0].status = 0;
    for(auto& group:allocator.noteGroups) group.next=group.previous=255;
    allocator.partHead.fill(255); allocator.partTail.fill(255);
    allocator.allocations[0].noteGroup = 0; allocator.allocations[0].part = 0;
    allocator.groups.head[0] = allocator.groups.tail[0] = 0;
    allocator.partHead[0] = allocator.partTail[0] = 0;
    allocator.partVoiceCount[0] = 1;
    allocator.partMinimum[0] = allocator.noteGroups[0].key = 60;
    allocator.freeTail = 24; // invalid, transactional failure
    require(!allocator.returnVoice(0) && allocator.allocations[0].status == 0 && allocator.groups.head[0] == 0);
    allocator.freeTail = 255;
    require(allocator.returnVoice(0) && allocator.freeHead == 0 && allocator.freeTail == 0
        && allocator.freeCount == 1 && allocator.freeGroupHead == 0 && allocator.partVoiceCount[0] == 0
        && allocator.partHead[0] == 255 && allocator.partTail[0] == 255 && allocator.partMinimum[0] == 127);
    require(allocator.returnVoice(0) && allocator.freeCount == 1); // double return is a no-op
    auto cyclic = allocator;
    cyclic.allocations[0].status = 0; cyclic.freeHead = cyclic.freeTail = 255; cyclic.freeCount = 0;
    cyclic.partMinimum[0] = cyclic.noteGroups[0].key = 60;
    cyclic.noteGroups[0].next = 1; cyclic.noteGroups[1].next = 1;
    cyclic.noteGroups[0].previous = 255; cyclic.partHead[0] = 0;
    require(!cyclic.returnVoice(0) && cyclic.freeCount == 0 && cyclic.allocations[0].status == 0
        && cyclic.partHead[0] == 0 && cyclic.noteGroups[0].next == 1);
    sc55::VoiceGroupLinks groups;
    sc55::VoiceLinks groupPcm;
    require(!groups.detach(24,0,groupPcm) && !groups.detach(0,24,groupPcm));
    groups.next[0] = 24;
    const auto beforeNext = groups.next;
    require(!groups.detach(0,0,groupPcm) && groups.next == beforeNext && groups.head[0] == 255);
    groups.next[0] = 255; groups.previous[0] = 24;
    require(!groups.detach(0,0,groupPcm) && groups.previous[0] == 24 && groups.head[0] == 255);
    groups.previous[0] = 128;
    require(groups.detach(0,0,groupPcm) && groups.tail[0] == 128 && groups.head[0] == 255);
    groups.next[0] = 1; groups.previous[1] = 0; groups.head[0] = 0; groups.tail[0] = 1;
    groupPcm.second[0] = 1; groupPcm.first[1] = 0;
    require(groups.detach(0,0,groupPcm) && groups.head[0] == 1 && groups.tail[0] == 1
        && groups.previous[1] == 255 && groupPcm.second[0] == 255 && groupPcm.first[1] == 255);
    sc55::VoiceLinks links;
    require(links.detach(0) && !links.detach(24));
    links.first[0] = 1; links.second[0] = 2;
    links.first[1] = 0; links.second[1] = 3; links.first[2] = 0;
    require(links.detach(0) && links.first[0] == 255 && links.second[0] == 255
        && links.first[1] == 255 && links.second[1] == 255 && links.first[2] == 0);
    links.second[0] = 0;
    require(links.detach(0) && links.second[0] == 255);
    links.first[0] = 24; links.second[0] = 2;
    require(!links.detach(0) && links.first[0] == 24 && links.second[0] == 2);
    const Runner::Setup setup{{{{{0,0},{1,0},{2,4},{3,4},{4,4}}},256,256},{100,80,60,40},256,256,2};
    const Runner::State initial{{Stage::attack1,{0,0},{0,0},0,100},0,0xff00,65534};
    sc55::EnvelopeTimes times{};
    const auto primed = Runner::start(setup,Stage::delay,0,64,times);
    require(primed && primed->state().level == 25600 && primed->state().pcmWord == 0x64ba);
    require(primed->state().segment.stage == Stage::delay && primed->state().delayAccumulator == 65535);
    require(primed->setup().targets[3] == 40 && primed->setup().delayIncrement == 0);
    auto activated = *primed;
    activated.activateNewVoice();
    require(activated.state().segment.stage == Stage::attack1 && activated.state().segment.progress.position == 65535);
    require(activated.state().pcmWord == 0x64ba);
    const auto specialDelay = Runner::start(setup,Stage::delay,128,64,times);
    require(specialDelay && specialDelay->setup().targets[3] == 0 && specialDelay->state().delayAccumulator == 0);
    activated = *specialDelay;
    activated.activateNewVoice();
    require(activated.state().segment.stage == Stage::delay && activated.state().segment.progress.position == 0);
    require(activated.state().pcmWord == 0x00b6 && activated.state().level == specialDelay->state().level);
    require(!Runner::start(setup,Stage::delay,0,128,times));
    times[1] = 12;
    const auto timedDelay = Runner::start(setup,Stage::delay,1,64,times);
    require(timedDelay && timedDelay->setup().delayIncrement == 32767 && timedDelay->setup().targets[3] == 0);
    times[1] = 65535; // H8 addition wraps before dividing by eight.
    const auto wrappedDelay = Runner::start(setup,Stage::delay,1,64,times);
    require(wrappedDelay && wrappedDelay->state().delayAccumulator == 65535);
    times.fill(0);
    Runner runner(setup,initial);
    runner.synchronizePcmLevel(1234);
    require(runner.state().level == 1234 && runner.state().segment.stage == initial.segment.stage
        && runner.state().segment.progress.position == initial.segment.progress.position
        && runner.state().segment.progress.deferredTicks == initial.segment.progress.deferredTicks
        && runner.state().pcmWord == initial.pcmWord && runner.state().segment.target == initial.segment.target);
    runner.synchronizePcmLevel(0);
    unsigned io = 0;
    // Hold writes the cached half-scale level; it must not perform a read.
    runner.synchronizePcmLevel(0x1235);
    sc55::SynchronizeEnvelopePcm(runner,[&](uint8_t) -> uint8_t { require(false); return 0; },
        [&](uint8_t address,uint8_t value) {
            require(address == 0x34+io && value == (io == 0 ? 0x09 : 0x1a)); ++io;
        });
    require(io == 2 && runner.state().level == 0x1235);
    auto moving = *primed; io = 0;
    sc55::SynchronizeEnvelopePcm(moving,[&](uint8_t address) -> uint8_t {
        require(io >= 2 && io <= 4 && address == (io == 2 ? 0x34 : io == 3 ? 0x3a : 0x3b));
        return ++io == 4 ? 0x92 : 0x34;
    },[&](uint8_t address,uint8_t value) {
        require(io < 2 && address == 0x18+io && value == (io == 0 ? 0xff : 0)); ++io;
    });
    require(io == 5 && moving.state().level == 0x2468 && moving.state().pcmWord == primed->state().pcmWord);
    for (unsigned raw = 0; raw < 65536; ++raw)
        require(sc55::DecodeEnvelopePcmLevel(uint16_t(raw)) == uint16_t(raw*2));
    runner.synchronizePcmLevel(0);
    // Entire note lifetime with no firmware state refreshed between ticks.
    for (unsigned stage = 1; stage <= 4; ++stage)
    {
        require(runner.tick(1,{},times));
        require(unsigned(runner.state().segment.stage) == stage);
        require(runner.state().level == uint16_t(setup.targets[stage-1] << 8));
        require(runner.state().segment.progress.position == 65535);
    }
    require(runner.tick(1,{},times));
    require(runner.state().segment.stage == Stage::sustain && runner.state().pcmWord == 0xff00);
    for (unsigned tick = 0; tick < 1000; ++tick) require(runner.tick(65535,{},times));
    require(runner.state().segment.stage == Stage::sustain && runner.state().level == 10240);
    runner.release(0x2345);
    require(runner.state().segment.start == 0x23 && runner.state().segment.stage == Stage::release);
    require(runner.tick(1,{},times) && runner.state().level == 0);
    require(runner.tick(1,{},times) && runner.state().segment.stage == Stage::finished);
    require(runner.tick(1,{},times) && runner.state().segment.stage == Stage::finished);

    auto delayed = initial;
    delayed.segment.stage = Stage::delay;
    Runner delay(setup,delayed);
    require(delay.tick(0,{},times) && delay.state().segment.stage == Stage::attack1);
    require(delay.state().delayAccumulator == 0 && delay.state().level == 0);
    require(delay.tick(0,{},times) && delay.state().level == 25600);
    Runner cancelled(setup,delayed);
    cancelled.release(0);
    require(cancelled.state().segment.stage == Stage::finished);

    times.fill(16);
    Runner slow(setup,initial);
    require(!slow.tick(1,{128,64,64},times) && slow.state().segment.progress.position == 0);
    require(slow.tick(1,{},times) && slow.state().segment.progress.position == 32768);
    require(slow.tick(2,{},times) && slow.state().segment.progress.deferredTicks == 1);
    require(slow.tick(0,{},times) && slow.state().segment.stage == Stage::attack2);
    require(slow.state().segment.progress.position == 32768 && slow.state().segment.progress.deferredTicks == 0);
}
