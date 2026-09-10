#pragma once
#include "allocator-state-snapshot.h"
#include "sc55_envelope_pcm.h"
#include "sc55_voice_lifecycle.h"
#include "sc55_rhythm_admission.h"
#include "sc55_note_setup.h"
#include "sc55_pitch.h"
#include "sc55_lfo.h"
#include "sc55_control_clock.h"
#include "sc55_note_dispatch.h"
#include "sc55_note_start.h"
#include "pcm.h"
#include <memory>
#include <stdexcept>
#include <source_location>

// Real PCM register implementation, no control ROM and no MCU instruction
// execution. The MCU object supplies device configuration only.
inline int verifyNativeEnvelopePcm()
{
    const auto require = [](bool ok,std::source_location where = std::source_location::current()) {
        if (!ok) throw std::runtime_error("Native PCM envelope integration regression at line "+std::to_string(where.line()));
    };
    auto mcu = std::make_unique<mcu_t>();
    {
        sc55::VoiceLinks links;
        uint8_t activity = 99;
        unsigned accesses = 0;
        const auto read = [&](uint8_t) { ++accesses; return uint8_t(0); };
        const auto write = [&](uint8_t,uint8_t) { ++accesses; };
        for (uint16_t stage : {uint16_t(0),uint16_t(12),uint16_t(18),uint16_t(20),uint16_t(22)})
            require(sc55::PollEnvelopeTermination(0,stage,activity,links,read,write) == sc55::EnvelopeTermination::bypassed);
        uint16_t stage = 14;
        require(!sc55::PollEnvelopeTermination(24,stage,activity,links,read,write));
        require(accesses == 0 && activity == 99 && stage == 14);
    }
    {
        sc55::PeriodicVoiceUpdatePass pass;
        std::array<uint16_t,24> stages; stages.fill(18); stages[23] = stages[7] = 0;
        sc55::VoiceLinks links; links.first[23] = 7;
        std::vector<unsigned> calls;
        const auto controllers = [&](const auto&) { calls.push_back(1); return true; };
        const auto first = [&](unsigned slot) { calls.push_back(100+slot); return true; };
        const auto paired = [&](unsigned destination,unsigned source) { require(destination == 23 && source == 7); calls.push_back(2); return true; };
        using Outcome = sc55::PeriodicVoiceUpdatePass::UpdateResult;
        const auto update = [&](unsigned slot) { require(pass.visited()[slot] == 255); calls.push_back(200+slot); return Outcome::proceed; };
        const auto write = [&](unsigned slot) { calls.push_back(300+slot); return true; };
        using Result = sc55::PeriodicVoiceUpdatePass::Result;
        require(pass.step(stages,links,controllers,first,paired,update,write) == Result::updated);
        require(calls == std::vector<unsigned>({1,107,2,207,223,307,323}));
        require(pass.step(stages,links,controllers,first,paired,update,write) == Result::complete);
        require(calls.size() == 7 && pass.visited()[7] == 0 && pass.visited()[23] == 0);
        for (auto stopped : {7u,23u})
        {
            pass.reset(); calls.clear();
            const auto stoppingUpdate = [&](unsigned slot) {
                update(slot);
                return slot == stopped ? Outcome::skipRemaining : Outcome::proceed;
            };
            require(pass.step(stages,links,controllers,first,paired,stoppingUpdate,write) == Result::updated);
            require(calls == (stopped == 7 ? std::vector<unsigned>{1,107,2,207}
                                           : std::vector<unsigned>{1,107,2,207,223}));
            require(pass.cursor() == 22 && pass.visited()[7] == 255);
            require(pass.visited()[23] == (stopped == 7 ? 0 : 255));
            require(pass.step(stages,links,controllers,first,paired,stoppingUpdate,write) == Result::complete);
        }
        pass.reset(); calls.clear();
        require(pass.step(stages,links,controllers,first,paired,update,[](unsigned) { return false; }) == Result::invalidInput);
        const auto count = calls.size();
        require(pass.step(stages,links,controllers,first,paired,update,write) == Result::invalidInput && calls.size() == count);
    }
    {
        sc55::FirstModulationVoice source,destination;
        std::array<uint16_t,128> depths{}; depths[5] = 1000;
        source.block.delay = source.block.attack = 65535;
        source.block.wave.phase = 12345; source.block.rateModifier = -19;
        source.block.depth = {3000,4000,5000}; source.sharing = {9,10,255};
        destination.block.depth = {100,200,300}; destination.sharing = {4,55,0};
        destination.block.delayRate = 17; destination.block.attackRate = 29;
        destination.block.waveform = 3; destination.block.rateIndex = 71;
        require(sc55::UpdatePairedFirstModulation(destination,source,5,64,depths));
        require(destination.block.depth[0] == 100 && destination.block.depth[1] == 200);
        require(destination.block.output[0] == 100 && destination.block.output[1] == 200);
        require(destination.block.wave.phase == 12345 && destination.block.rateModifier == -19);
        require(destination.block.delayRate == 17 && destination.block.attackRate == 29
            && destination.block.waveform == 3 && destination.block.rateIndex == 71);
        require(destination.sharing.source == 4 && destination.sharing.baseRate == 55 && destination.sharing.sharing == 255);
        require(!sc55::UpdatePairedFirstModulation(destination,source,5,128,depths));
        require(destination.block.output[0] == 100);
        require(sc55::UpdatePairedFirstModulation(destination,destination,5,64,depths));
        require(destination.block.wave.phase == 12345);
        source.block.delay = 65534;
        require(sc55::UpdatePairedFirstModulation(destination,source,5,64,depths));
        for (auto output : destination.block.output) require(output == 0);
    }
    {
        sc55::VoiceInstallationState installed;
        sc55::PartControllerState parts;
        std::array<sc55::VoiceControllerState,24> controllers{};
        installed.voices[3].input.part = 2; installed.voices[3].input.originalKey = 60;
        installed.voices[7].input.part = 255; // partner metadata must not be used to recalculate
        parts.parts[2].keyPressure[60] = 96;
        parts.parts[2].sensitivity = {65,64,64,64,0,0,0,64,0,0,0};
        const sc55::VoiceUpdateSelection pair{{3,7},2,2};
        require(sc55::RefreshSelectedVoiceControllers(pair,installed,parts,controllers));
        require(controllers[3].pitchOffset != 0 && controllers[7].pitchOffset == controllers[3].pitchOffset);
        require(controllers[0].pitchOffset == 0);
        const auto previous = controllers[3].pitchOffset;
        parts.parts[2].keyPressure[60] = 0;
        auto invalid = pair; invalid.slots[1] = 24;
        require(!sc55::RefreshSelectedVoiceControllers(invalid,installed,parts,controllers));
        require(controllers[3].pitchOffset == previous);
        installed.voices[3].input.originalKey = 128;
        require(!sc55::RefreshSelectedVoiceControllers(pair,installed,parts,controllers));
        require(controllers[7].pitchOffset == previous);
        require(sc55::RefreshSelectedVoiceControllers({},installed,parts,controllers));
    }
    {
        std::array<uint16_t,24> stages; stages.fill(18);
        std::array<uint8_t,24> visited{};
        sc55::VoiceLinks links;
        stages[23] = 17; stages[8] = 0;
        links.first[23] = 8; links.second[23] = 254;
        const auto pair = sc55::SelectNextVoiceUpdate(23,stages,visited,links);
        require(pair && pair->count == 2 && pair->slots[0] == 8 && pair->slots[1] == 23 && pair->resume == 22);
        visited[23] = visited[8] = 1;
        const auto done = sc55::SelectNextVoiceUpdate(pair->resume,stages,visited,links);
        require(done && done->count == 0);
        for (auto v : visited) require(v == 0);
        links.first[23] = 255; // selected malformed second link must reject without clearing flags
        visited[8] = 1;
        require(!sc55::SelectNextVoiceUpdate(23,stages,visited,links) && visited[8] == 1);
        require(!sc55::SelectNextVoiceUpdate(24,stages,visited,links));
        links.second[23] = 23; // firmware permits self pairing
        const auto self = sc55::SelectNextVoiceUpdate(23,stages,visited,links);
        require(self && self->count == 2 && self->slots[0] == 23 && self->slots[1] == 23);
        const auto end = sc55::SelectNextVoiceUpdate(255,stages,visited,links);
        require(end && end->count == 0 && visited[8] == 0);
    }
    {
        sc55::PartControllerState state;
        std::array<sc55::PartMidiReceive,16> routing{};
        routing[1] = {3,0x4000,0}; routing[4] = {3,0x4000,0}; routing[8] = {3,0x3fff,0};
        for (auto& part : state.parts)
            part.sourceSensitivity[1] = {65,63,64,64,1,0,0,64,0,0,0};
        sc55::MidiDecoder decoder;
        const auto receive = [&](const auto& e) { require(state.receivePitchBend(e,routing)); };
        for (auto byte : std::array<uint8_t,3>{0xe3,127,127}) decoder.push(std::span(&byte,1),receive);
        const auto positive = state.parts[1].contributions[1];
        require(positive[0] > 0 && positive[0] < 32768 && positive[1] > 32768 && positive[4] > 32768);
        require(state.parts[4].contributions[1] == positive && state.parts[8].contributions[1][0] == 0);
        decoder.push(std::array<uint8_t,2>{0,0},receive);
        const auto negative = state.parts[1].contributions[1];
        require(negative[0] == uint16_t(0u-positive[0]) && negative[4] == uint16_t(0u-positive[4]));
        decoder.push(std::array<uint8_t,2>{1,0},receive);
        require(state.parts[1].contributions[1] == negative); // raw zero clamps to one
        decoder.push(std::array<uint8_t,2>{0,64},receive);
        for (auto value : state.parts[1].contributions[1]) require(value == 0);
        const sc55::MidiDecoder::Event invalid{sc55::MidiDecoder::Kind::message,0xe3,128,0,2};
        require(!state.receivePitchBend(invalid,routing));
    }
    {
        sc55::PartControllerState state;
        std::array<sc55::PartMidiReceive,16> routing{};
        for (unsigned p : {13u,14u,15u})
        {
            routing[p] = {3,0x802,0};
            state.parts[p].assignedControllers = {1,1};
            for (auto& row : state.parts[p].sourceSensitivity)
                row = {65,64,64,64,0,0,0,64,0,0,0};
        }
        state.parts[14].assignedControllers[1] = 2;
        sc55::MidiDecoder decoder;
        const auto receive = [&](const auto& e) { require(state.receiveControlContributions(e,routing)); };
        const std::array<uint8_t,3> bytes{0xb3,1,97};
        for (auto byte : bytes) decoder.push(std::span(&byte,1),receive);
        require(state.parts[15].contributions[0][0] == 97 && state.parts[15].contributions[3][0] == 97
            && state.parts[15].contributions[4][0] == 97);
        require(state.parts[13].contributions[3][0] == 97 && state.parts[13].contributions[4][0] == 0);
        require(state.parts[14].contributions[4][0] == 0); // mismatch switches remaining scan
        const std::array<uint8_t,2> clear{1,0}; decoder.push(clear,receive);
        require(state.parts[15].contributions[0][0] == 0 && state.parts[15].contributions[4][0] == 0);
        routing[15].flags = 0x800; // modulation disabled, assignable CC still enabled
        decoder.push(std::array<uint8_t,2>{1,31},receive);
        require(state.parts[15].contributions[0][0] == 0 && state.parts[15].contributions[4][0] == 31);
        state.parts[15].assignedControllers = {121,121};
        decoder.push(std::array<uint8_t,2>{121,99},receive);
        require(state.parts[15].contributions[4][0] == 31);
        const sc55::MidiDecoder::Event invalid{sc55::MidiDecoder::Kind::message,0xb3,1,128,2};
        require(!state.receiveControlContributions(invalid,routing));
    }
    {
        sc55::PartControllerState state;
        std::array<sc55::PartMidiReceive,16> routing{};
        routing[1] = {3,0x2000,0}; routing[4] = {3,0x2000,0}; routing[8] = {3,0x0fff,0};
        for (auto& part : state.parts)
            part.sourceSensitivity[2] = {65,63,64,64,0,0,0,64,0,0,0};
        sc55::MidiDecoder decoder;
        const auto receive = [&](const auto& event) { require(state.receiveChannelPressure(event,routing)); };
        const std::array<uint8_t,3> bytes{0xd3,96,3};
        for (const auto byte : bytes) decoder.push(std::span(&byte,1),receive);
        require(state.parts[1].contributions[2][0] == 3);
        require(state.parts[4].contributions[2][1] == 65535); // -3/2 truncates to -1
        require(state.parts[8].contributions[2][0] == 0);
        require(sc55::PrepareVoiceControllers(*state.inputs(1,60)).pitchOffset > 0);
        const uint8_t clear = 0; decoder.push(std::span(&clear,1),receive);
        require(sc55::PrepareVoiceControllers(*state.inputs(1,60)).pitchOffset == 0);
        sc55::MidiDecoder::Event invalid{sc55::MidiDecoder::Kind::message,0xd3,128,0,1};
        require(!state.receiveChannelPressure(invalid,routing));
        invalid.first = 3; invalid.dataSize = 2;
        require(!state.receiveChannelPressure(invalid,routing));
    }
    {
        sc55::PartControllerState state;
        std::array<sc55::PartMidiReceive,16> routing{};
        routing[1] = {3,0x400,0}; routing[4] = {3,0x400,0}; routing[8] = {3,0,0};
        sc55::MidiDecoder decoder;
        const std::array<uint8_t,5> bytes{0xa3,60,96,61,20}; // running status across one-byte packets
        for (const auto byte : bytes)
            decoder.push(std::span(&byte,1),[&](const auto& event) { require(state.receivePolyPressure(event,routing)); });
        require(state.parts[1].keyPressure[60] == 96 && state.parts[4].keyPressure[61] == 20);
        require(state.parts[8].keyPressure[60] == 0 && state.parts[1].keyPressure[59] == 0);
        state.parts[1].sensitivity = {65,64,64,64,0,0,0,64,0,0,0};
        const auto input = state.inputs(1,60);
        require(input && input->keyValue == 96 && sc55::PrepareVoiceControllers(*input).pitchOffset > 0);
        const sc55::MidiDecoder::Event clear{sc55::MidiDecoder::Kind::message,0xa3,60,0,2};
        require(state.receivePolyPressure(clear,routing));
        require(sc55::PrepareVoiceControllers(*state.inputs(1,60)).pitchOffset == 0);
        auto invalid = clear; invalid.first = 128;
        require(!state.receivePolyPressure(invalid,routing));
        require(!state.inputs(16,60) && !state.inputs(1,128));
    }
    {
        sc55::VoiceControllerInputs input;
        input.keyValue = 127;
        input.sensitivity = {64,64,64,64,0,0,0,64,0,0,0};
        const auto zero = sc55::PrepareVoiceControllers(input);
        require(zero.pitchOffset == 0 && zero.envelopeOffset == 0 && zero.levelBias == 0);
        for (unsigned i = 0; i < 2; ++i)
            require(!zero.rateModifiers[i] && !zero.levelDepths[i] && !zero.envelopeDepths[i] && !zero.pitchDepths[i]);
        // Word wrapping happens before sign/saturation, so cancellation is exact.
        input.contributions[0].fill(65535); input.contributions[1].fill(1);
        require(sc55::PrepareVoiceControllers(input).pitchOffset == 0);
        input.contributions[1].fill(2);
        const auto small = sc55::PrepareVoiceControllers(input);
        require(small.pitchOffset == 7 && small.envelopeOffset == 6 && small.levelBias == 8);
        require(small.rateModifiers[0] == 1 && small.pitchDepths[0] == 1 && small.envelopeDepths[0] == 1 && small.levelDepths[0] == 8);
    }
    {
        sc55::VoiceControllerState source{0xffff,0x8000,0xfffe,{0x8001,32767},{0xfffd,4},{5,6},{7,8}}, destination;
        sc55::CopyPairedVoiceControllers(destination,source);
        sc55::CopyPairedVoiceControllers(destination,destination);
        sc55::LevelInputs level; level.master = 100; level.mod1_a = 11;
        sc55::SecondEnvelopeOutputInputs envelope; envelope.sources[0].first = 12;
        sc55::PitchModulationInputs pitch; pitch.sources[0].waveform = 13;
        sc55::ModulationBlock first, second;
        first.wave.phase = 14; second.wave.phase = 15; first.depth[0] = 16;
        destination.apply(level,envelope,pitch,first,second);
        require(pitch.offset == 65535 && envelope.offset == 32768 && level.bias == -2);
        require(level.mod1_b == -3 && level.mod2_b == 4 && level.mod1_a == 11 && level.master == 100);
        require(first.rateModifier == -32767 && second.rateModifier == 32767);
        require(envelope.sources[0].second == 5 && envelope.sources[1].second == 6 && envelope.sources[0].first == 12);
        require(pitch.sources[0].second == 7 && pitch.sources[1].second == 8 && pitch.sources[0].waveform == 13);
        require(first.wave.phase == 14 && second.wave.phase == 15 && first.depth[0] == 16);
    }
    {
        sc55::InstalledVoice installed{{12,345,1,2,60,64,100,0,67,false},32,sc55::VoiceOperation::prepare};
        uint8_t activity = 7;
        auto context = sc55::PrepareVoiceContext(23,installed,activity);
        using Table = sc55::VoicePreparationContext::KeyTable;
        require(context && context->tone == 12 && context->sample == 345 && context->part == 2);
        require(context->keyTable == Table::first && context->keyIndex == 67 && activity == 7);
        installed.flags = 160; installed.input.sampleMode = 127;
        context = sc55::PrepareVoiceContext(23,installed,activity);
        require(context && context->flags == 160 && context->keyTable == Table::second && activity == 255);
        installed.input.sampleMode = 128;
        context = sc55::PrepareVoiceContext(23,installed,activity);
        require(context && context->keyTable == Table::none);
        activity = 9; installed.input.sample = 0xffff;
        require(!sc55::PrepareVoiceContext(23,installed,activity) && activity == 9);
        installed.input.sample = 345;
        require(!sc55::PrepareVoiceContext(24,installed,activity) && activity == 9);
    }
    {
        std::array<sc55::VoiceStopState,24> voices{};
        sc55::VoiceLinks links;
        std::array<uint8_t,24> activity; activity.fill(7);
        const auto idle = sc55::DispatchNextVoiceTask(voices,links,activity);
        require(idle && idle->kind == sc55::VoiceTaskDispatch::Kind::idle && !idle->count);
        voices[3].pendingOperation = voices[20].pendingOperation = sc55::VoiceOperation::prepare;
        links.first[20] = 3; links.second[20] = 24; // ignored when first exists
        const auto paired = sc55::DispatchNextVoiceTask(voices,links,activity);
        require(paired && paired->slots == std::array<uint8_t,2>{3,20} && paired->count == 2);
        require((voices[3].pendingOperation == sc55::VoiceOperation::none) && (voices[20].pendingOperation == sc55::VoiceOperation::none) && activity[20] == 7);
        voices[20].pendingOperation = sc55::VoiceOperation::prepare; links.first[20] = 255;
        require(!sc55::DispatchNextVoiceTask(voices,links,activity) && voices[20].pendingOperation == sc55::VoiceOperation::prepare);
        voices[20].pendingOperation = static_cast<sc55::VoiceOperation>(3);
        require(!sc55::DispatchNextVoiceTask(voices,links,activity) && voices[20].pendingOperation == static_cast<sc55::VoiceOperation>(3));
        voices[20].pendingOperation = sc55::VoiceOperation::finishStop; voices[20].stages = {18,99,99};
        const auto stopped = sc55::DispatchNextVoiceTask(voices,links,activity);
        require(stopped && stopped->kind == sc55::VoiceTaskDispatch::Kind::finishStop);
        require(voices[20].stages == std::array<uint16_t,3>{14,14,14} && activity[20] == 0);
    }
    {
        sc55::VoiceAllocator allocator; require(allocator.initializeTables());
        auto group = allocator.createGroup({1,0x80,60,1,1}); require(bool(group));
        const auto slot = group->voices[0];
        sc55::VoiceInstallationState installation;
        installation.pendingRelease[slot] = 255; allocator.allocations[slot].releaseCommand = 255;
        sc55::VoiceInstallationInput input{12,345,1,1,60,64,100,3,67,true};
        uint8_t flags = 0x40;
        require(installation.install(slot,input,flags,allocator));
        require(flags == 0xc0 && installation.voices[slot].input == input);
        require(installation.voices[slot].operation == sc55::VoiceOperation::prepare && allocator.allocations[slot].status == 0);
        require(installation.pendingRelease[slot] == 0 && allocator.allocations[slot].releaseCommand == 0);
        auto invalid = input; invalid.part = 16;
        require(!installation.install(slot,invalid,flags,allocator));
        invalid = input; invalid.partial = 2;
        require(!installation.install(slot,invalid,flags,allocator));
        require(!installation.install(24,input,flags,allocator));
        installation.pendingRelease[slot] = 99;
        auto absent = input; absent.sample = 0xffff;
        flags = 1;
        require(installation.install(slot,absent,flags,allocator));
        require(flags == 1 && installation.voices[slot].input == input);
        require(installation.pendingRelease[slot] == 99 && allocator.freeCount == 24);
        // A corrupt return must roll back the initial status clear too.
        allocator.allocations[slot].noteGroup = 24;
        const auto status = allocator.allocations[slot].status;
        require(!installation.install(slot,absent,flags,allocator));
        require(allocator.allocations[slot].status == status && installation.pendingRelease[slot] == 99);
    }
    {
        SC55Patch patch;
        const auto both = sc55::PlanPartialVoiceDispatch(patch,3,{4,255});
        require(both && (*both)[0].prepare && (*both)[1].prepare);
        require((*both)[0].voice == 4 && (*both)[1].voice == 4);
        const auto absent = sc55::PlanPartialVoiceDispatch(patch,3,{255,255});
        require(absent && (*absent)[0].prepare && (*absent)[1].prepare);
        require((*absent)[0].voice == 255 && (*absent)[1].voice == 255);
        patch.partial[0].raw[2] = patch.partial[0].raw[3] = 255;
        const auto skipped = sc55::PlanPartialVoiceDispatch(patch,3,{24,7});
        require(skipped && !(*skipped)[0].prepare && (*skipped)[1].voice == 7);
        require(!sc55::PlanPartialVoiceDispatch(patch,3,{24,255}));
        const auto disabled = sc55::PlanPartialVoiceDispatch(patch,0,{24,24});
        require(disabled && !(*disabled)[0].prepare && !(*disabled)[1].prepare);
        sc55::PartialDispatchState state;
        const sc55::PartialVoiceInputs first{999,127,64,255}, second{1,2,3,60};
        require(state.stage(1,0,(*both)[0],60,first));
        require(state.stage(1,1,(*both)[1],64,second));
        require(state.voices[4] == second && state.previousKeys[1][0] == 60 && state.previousKeys[1][1] == 64);
        require(state.stage(1,0,{true,255},67,first));
        require(state.previousKeys[1][0] == 67 && state.voices[4] == second);
        require(state.stage(1,0,{false,24},99,first));
        require(!state.stage(1,0,{true,24},99,first));
        require(!state.stage(16,0,{true,4},99,first));
        require(!state.stage(1,2,{true,4},99,first));
        require(state.previousKeys[1][0] == 67 && state.voices[4] == second);
    }
    {
        sc55::VoiceAllocator allocator; require(allocator.initializeTables());
        const auto group = allocator.createGroup({0,0x80,60,1,2}); require(group.has_value());
        const auto tail = allocator.groups.tail[group->group], head = allocator.groups.head[group->group];
        allocator.allocations[tail].status = allocator.allocations[head].status = 0;
        const sc55::VoiceAllocator::MonoReuse input{255,{255,17}};
        const auto active = allocator.prepareMonoReuse(0,input);
        require(active && active->flags == 0x5f && active->voices[0] == tail && active->voices[1] == head);
        allocator.allocations[head].status = 0x94;
        const auto restart = allocator.prepareMonoReuse(0,input);
        require(restart && restart->flags == 0xdf);
        allocator.groups.head[group->group] = tail;
        const auto single = allocator.prepareMonoReuse(0,input);
        require(single && single->flags == 0x5f && single->voices[1] == 17);
        allocator.groups.head[group->group] = 255;
        const auto missing = allocator.prepareMonoReuse(0,input);
        require(missing && missing->voices[1] == 255 && missing->flags == 0x5f);
        allocator.groups.head[group->group] = 24;
        require(!allocator.prepareMonoReuse(0,input));
        require(!allocator.prepareMonoReuse(16,input));
    }
    {
        SC55Patch patch;
        sc55::VelocityCurves curves{};
        for (auto& curve : curves) curve.fill(127);
        patch.common[6] = 3;
        for (auto& partial : patch.partial)
        { partial.raw[0x41] = 0; partial.raw[0x43] = 127; partial.raw[0x44] = 127; }
        const sc55::MonoHeldKeys::ReleaseDecision decision{
            sc55::MonoHeldKeys::ReleaseDecision::Action::replaceKey,60};
        for (unsigned velocity = 0; velocity < 128; ++velocity)
            for (bool soft : {false,true})
            {
                const auto result = sc55::PrepareMonoReplacementVelocity(decision,patch,uint8_t(velocity),soft,curves);
                require(result && result->candidates.count == 2);
                const auto expected = sc55::PreparePatchVelocity(3,uint8_t(velocity),
                    patch.partial[0].raw,patch.partial[1].raw,0,soft,curves);
                require(result->accumulator == expected.accumulator);
                for (unsigned i = 0; i < 2; ++i)
                    require(result->partials[i]->amplitude == expected.partials[i]->amplitude
                        && result->partials[i]->secondary == expected.partials[i]->secondary);
            }
        patch.common[6] = 0;
        const auto rejected = sc55::PrepareMonoReplacementVelocity(decision,patch,110,false,curves);
        require(rejected && rejected->candidates.count == 0 && rejected->accumulator == 0);
        require(!sc55::PrepareMonoReplacementVelocity(decision,patch,128,false,curves));
        require(!sc55::PrepareMonoReplacementVelocity(
            {sc55::MonoHeldKeys::ReleaseDecision::Action::releaseGroup},patch,110,false,curves));
    }
    {
        sc55::MonoHeldKeys keys;
        using Action = sc55::MonoHeldKeys::ReleaseDecision::Action;
        require(keys.set(60,true) && keys.set(64,true) && keys.set(67,true));
        const auto noncurrent = keys.release(64,67);
        require(noncurrent && noncurrent->action == Action::unchangedVoice && keys.highest() == 67);
        const auto replacement = keys.release(67,67);
        require(replacement && replacement->action == Action::replaceKey && replacement->replacement == 60);
        sc55::VoiceAllocator allocator; require(allocator.initializeTables());
        const auto group = allocator.createGroup({0,0x80,60,1,2}); require(group.has_value());
        const auto last = keys.release(60,60);
        require(last && last->action == Action::releaseGroup && !keys.highest());
        require(allocator.releaseMonoGroup(0));
        require(allocator.allocations[group->voices[0]].releaseCommand == 255 && allocator.allocations[group->voices[1]].releaseCommand == 255);
        const auto before = keys.words;
        require(!keys.release(128,60) && keys.words == before);
        // Current key comparison is independent of whether its bit was set.
        const auto duplicate = keys.release(60,60);
        require(duplicate && duplicate->action == Action::releaseGroup);
    }
    {
        sc55::VoiceAllocator allocator; require(allocator.initializeTables());
        const auto group = allocator.createGroup({0,0x80,60,1,2}); require(group.has_value());
        const auto head = allocator.groups.head[group->group], tail = allocator.groups.tail[group->group];
        allocator.groups.previous[tail] = 255;
        allocator.noteGroups[group->group].status = 94;
        require(allocator.releaseMonoGroup(0));
        require(allocator.noteGroups[group->group].status == 2);
        require(allocator.allocations[head].releaseCommand == 255 && allocator.allocations[tail].releaseCommand == 255);
        for(auto& voice:allocator.allocations) voice.releaseCommand=0; allocator.partFlags[0] = 1;
        require(allocator.releaseMonoGroup(0));
        require(allocator.noteGroups[group->group].retirementFlags&1);
        require(allocator.allocations[head].releaseCommand == 0 && allocator.allocations[tail].releaseCommand == 0);
        allocator.partFlags[0] = 0; allocator.groups.head[group->group] = 24;
        require(!allocator.releaseMonoGroup(0));
        require(allocator.allocations[tail].releaseCommand == 0);
        require(!allocator.releaseMonoGroup(16));
        require(!allocator.releaseMonoGroup(1));
    }
    {
        sc55::MonoHeldKeys keys;
        require(!keys.highest());
        require(keys.set(72,true) && keys.set(60,true) && keys.highest() == 72);
        require(keys.set(72,true) && keys.set(72,false) && keys.highest() == 60);
        require(keys.set(60,false) && !keys.highest());
        const auto before = keys.words;
        require(!keys.set(128,true) && keys.words == before);
        for (unsigned key = 0; key < 128; ++key)
        { require(keys.set(key,true)); require(keys.highest() == key); }
        for (unsigned key = 128; key-- > 0;)
        {
            require(keys.highest() == key); require(keys.set(key,false));
        }
        require(!keys.highest());
    }
    {
        sc55::PartNoteState notes; require(notes.allocator.initializeTables());
        std::array<sc55::PartMidiReceive,16> routing;
        routing[0] = {7,0x0200,0x80}; routing[1] = {7,0x0200,0x10};
        const auto wrong = notes.allocator.createGroup({0,0x81,60,1,1});
        const auto normal = notes.allocator.createGroup({0,0x80,60,1,1});
        const auto rhythm = notes.allocator.createGroup({1,5,60,1,1});
        require(wrong && normal && rhythm);
        const sc55::MidiDecoder::Event off{sc55::MidiDecoder::Kind::message,0x87,60,0,2};
        require(notes.receiveNoteOff(off,routing,{}) == 3);
        require(notes.allocator.noteGroups[wrong->group].status == 0);
        require(notes.allocator.noteGroups[normal->group].status == 2);
        require(notes.allocator.noteGroups[rhythm->group].status == 2);
        const auto high = notes.allocator.createGroup({0,0x81,125,1,1}); require(high.has_value());
        routing[0].noteFlags = 0; // High-key branch precedes mono mode.
        const sc55::MidiDecoder::Event highOff{sc55::MidiDecoder::Kind::message,0x97,125,0,2};
        require(notes.receiveNoteOff(highOff,routing,{}) == 1);
        notes.allocator.noteGroups[rhythm->group].status = 0;
        // Part1 would release before unsupported mono part0: rollback both.
        require(!notes.receiveNoteOff(off,routing,{}).has_value());
        require(notes.allocator.noteGroups[rhythm->group].status == 0);
        require(!sc55::SelectNoteRelease(128,0x80));
    }
    for (unsigned part = 0; part < 16; ++part)
    {
        const sc55::PartMidiReceive enabled{7,0x0200}, disabled{7,0};
        require(sc55::AcceptNotePart(7,part,enabled,{}));
        require(!sc55::AcceptNotePart(6,part,enabled,{}));
        require(!sc55::AcceptNotePart(7,part,disabled,{}));
        require(!sc55::AcceptNotePart(7,part,enabled,{0,0,1}));
        require(sc55::AcceptNotePart(7,part,disabled,{8,uint8_t(part),1}));
        require(!sc55::AcceptNotePart(7,part,enabled,{8,uint8_t((part+1)%16),0}));
        require(sc55::AcceptNotePart(7,part,disabled,{10,255,1}));
        require(!sc55::AcceptNotePart(16,part,enabled,{}));
        require(!sc55::AcceptNotePart(7,16,enabled,{}));
    }
    {
        sc55::PartNoteState notes; require(notes.allocator.initializeTables());
        std::array<sc55::PartMidiReceive,16> routing;
        for (unsigned part = 0; part < 16; ++part)
        {
            routing[part] = {7,0x08a0};
            require(notes.allocator.createGroup({uint8_t(part),0,60,1,1}).has_value());
        }
        routing[0].channel = 255;
        routing[1].flags = 0x00a0; // Missing common receive permission.
        routing[2].flags = 0x0820; // Hold only.
        const sc55::MidiDecoder::Event on{sc55::MidiDecoder::Kind::message,0xb7,66,127,2};
        require(notes.receivePedal(on,routing));
        for (unsigned part = 0; part < 16; ++part)
            require(notes.part(part)->sostenutoEnabled == (part >= 3));
        // Descending fan-out reaches a corrupt lower part after valid higher
        // parts. The entire operation, including their clears, must roll back.
        notes.allocator.partHead[4] = 24;
        const sc55::MidiDecoder::Event off{sc55::MidiDecoder::Kind::message,0xb7,66,0,2};
        require(!notes.receivePedal(off,routing));
        for (unsigned part = 3; part < 16; ++part)
        { require(notes.part(part)->sostenutoEnabled); require(notes.part(part)->retainedKeys[0] == 60); }
        const sc55::MidiDecoder::Event unmatched{sc55::MidiDecoder::Kind::message,0xb6,66,0,2};
        require(notes.receivePedal(unmatched,routing));
    }
    {
        sc55::PartNoteState notes; require(notes.allocator.initializeTables());
        std::array<uint8_t,16> slots;
        sc55::MidiDecoder decoder;
        const auto pedal = [&](unsigned part,uint8_t cc,uint8_t value,bool receive = true) {
            const std::array<uint8_t,3> bytes{0xb7,cc,value};
            for (auto byte : bytes) decoder.push(std::span(&byte,1),[&](const auto& event) {
                require(notes.applyPedal(event,part,receive));
            });
        };
        for (unsigned part = 0; part < 16; ++part)
        {
            const auto created = notes.allocator.createGroup({uint8_t(part),0,60,1,1});
            require(created.has_value()); slots[part] = created->voices[0];
            require(notes.part(part)->retainedKeys[0] == 255);
            pedal(part,66,127);
            if (part&1) pedal(part,64,127);
            const sc55::MidiDecoder::Event off{sc55::MidiDecoder::Kind::message,0x87,60,0,2};
            require(notes.noteOff(off,part,0) == true);
            require(notes.allocator.allocations[slots[part]].releaseCommand == 0);
        }
        // All16 same-key notes coexist. Releasing one part must not release
        // another, and disabled receive must not clear that part's row.
        for (unsigned part = 0; part < 16; ++part)
        {
            pedal(part,66,0,false);
            require(notes.part(part)->sostenutoEnabled);
            require(notes.part(part)->retainedKeys[0] == 60);
            pedal(part,66,0);
            require(!notes.part(part)->sostenutoEnabled);
            require(notes.allocator.allocations[slots[part]].releaseCommand == ((part&1) ? 0 : 255));
            if (part&1) pedal(part,64,0);
            for (unsigned p = 0; p < 16; ++p)
            {
                require(notes.allocator.allocations[slots[p]].releaseCommand == (p <= part ? 255 : 0));
                require(notes.part(p)->retainedKeys[0] == (p <= part ? 255 : 60));
            }
        }
        require(notes.part(16) == nullptr);
        const sc55::MidiDecoder::Event off{sc55::MidiDecoder::Kind::message,0x80,60,0,2};
        require(!notes.noteOff(off,16,0).has_value());
        require(!notes.applyPedal(off,16,true));
    }
    for (unsigned part = 0; part < 16; ++part)
        for (unsigned value = 0; value < 128; ++value)
            for (bool receive : {false,true})
            {
                sc55::VoiceAllocator allocator; require(allocator.initializeTables());
                require(allocator.createGroup({uint8_t(part),0,60,1,1}).has_value());
                std::array<uint8_t,16> retained; retained.fill(255);
                bool enabled = false;
                sc55::MidiDecoder decoder;
                const std::array<uint8_t,3> bytes{uint8_t(0xb0|((part+3)%16)),66,uint8_t(value)};
                for (auto byte : bytes) decoder.push(std::span(&byte,1),[&](const auto& event) {
                    require(sc55::ApplyRoutedSostenutoController(event,part,receive,enabled,retained,allocator));
                });
                require(enabled == (receive && value >= 64));
                require(retained[0] == (enabled ? 60 : 255));
                require(allocator.createGroup({uint8_t(part),0,61,1,1}).has_value());
                for (auto byte : bytes) decoder.push(std::span(&byte,1),[&](const auto& event) {
                    require(sc55::ApplyRoutedSostenutoController(event,part,receive,enabled,retained,allocator));
                });
                require(retained[1] == (enabled ? 61 : 255));
                const auto before = retained;
                const sc55::MidiDecoder::Event bad{sc55::MidiDecoder::Kind::message,0xb0,66,128,2};
                require(!sc55::ApplyRoutedSostenutoController(bad,part,true,enabled,retained,allocator));
                require(retained == before);
            }
    {
        sc55::VoiceAllocator allocator; require(allocator.initializeTables());
        const auto first = allocator.createGroup({0,0,60,1,2});
        const auto second = allocator.createGroup({0,0,61,1,1});
        require(first.has_value() && second.has_value());
        std::array<uint8_t,16> retained; retained.fill(255);
        require(allocator.captureRetainedKeys(0,retained));
        require(allocator.requestNoteRelease(0,60,0,retained) == true);
        require(allocator.setPartHold(0,true,retained));
        require(allocator.requestNoteRelease(0,61,0,retained) == true);
        require(allocator.releaseRetainedKeys(0,retained));
        for (auto key : retained) require(key == 255);
        require(allocator.allocations[first->voices[0]].releaseCommand == 255);
        require(allocator.allocations[first->voices[1]].releaseCommand == 255);
        require(allocator.allocations[second->voices[0]].releaseCommand == 0);
        require(allocator.setPartHold(0,false,retained));
        require(allocator.allocations[second->voices[0]].releaseCommand == 255);

        for(auto& voice:allocator.allocations) voice.releaseCommand=0;
        retained.fill(61);
        // Full nonmatching row exits at the first group, skipping the second.
        require(allocator.releaseRetainedKeys(0,retained));
        require(allocator.allocations[second->voices[0]].releaseCommand == 0);
        retained.fill(255); retained[0] = 60; retained[1] = 61;
        const auto before = retained;
        allocator.groups.tail[second->group] = 24;
        require(!allocator.releaseRetainedKeys(0,retained));
        require(retained == before);
        for (const auto& voice : allocator.allocations) require(voice.releaseCommand == 0);
        require(!allocator.releaseRetainedKeys(16,retained));
    }
    {
        sc55::VoiceAllocator allocator; require(allocator.initializeTables());
        const auto first = allocator.createGroup({0,0,60,1,1});
        const auto duplicate = allocator.createGroup({0,0,60,1,1});
        const auto released = allocator.createGroup({0,0,61,1,1});
        require(first.has_value() && duplicate.has_value() && released.has_value());
        allocator.noteGroups[released->group].status = 2;
        std::array<uint8_t,16> retained; retained.fill(255);
        require(allocator.captureRetainedKeys(0,retained));
        require(retained[0] == 60 && retained[1] == 255);
        // Corruption after an otherwise valid insertion must not leak changes.
        retained.fill(255);
        allocator.noteGroups[released->group].next = first->group;
        require(!allocator.captureRetainedKeys(0,retained));
        for (auto key : retained) require(key == 255);
        require(!allocator.captureRetainedKeys(16,retained));
    }
    for (unsigned part = 0; part < 16; ++part)
        for (unsigned value = 0; value < 128; ++value)
            for (bool enabled : {false,true})
            {
                sc55::VoiceAllocator allocator; require(allocator.initializeTables());
                allocator.partFlags.fill(128);
                sc55::MidiDecoder decoder;
                std::array<uint8_t,16> retained; retained.fill(255);
                // Channel deliberately differs from destination part: the
                // routed API must not silently substitute status&15 for part.
                const std::array<uint8_t,3> bytes{uint8_t(0xb0|((part+7)%16)),64,uint8_t(value)};
                for (const auto byte : bytes)
                    decoder.push(std::span(&byte,1),[&](const auto& event) {
                        require(sc55::ApplyRoutedHoldController(event,part,enabled,retained,allocator));
                    });
                for (unsigned p = 0; p < 16; ++p)
                    require(allocator.partFlags[p] == (128|((p == part && enabled && value >= 64) ? 1 : 0)));
                const auto before = allocator.partFlags;
                auto bad = sc55::MidiDecoder::Event{sc55::MidiDecoder::Kind::message,0xb0,64,128,2};
                require(!sc55::ApplyRoutedHoldController(bad,part,true,retained,allocator));
                bad.second = 127; bad.first = 66;
                require(!sc55::ApplyRoutedHoldController(bad,part,true,retained,allocator));
                bad.first = 64;
                require(!sc55::ApplyRoutedHoldController(bad,16,false,retained,allocator));
                require(allocator.partFlags == before);
            }
    {
        sc55::VoiceAllocator allocator; require(allocator.initializeTables());
        std::array<uint8_t,16> retained; retained.fill(255);
        const auto first = allocator.createGroup({0,0,60,1,2});
        const auto second = allocator.createGroup({0,0,61,1,1});
        require(first && second);
        allocator.partFlags[0] = 128;
        require(allocator.setPartHold(0,true,retained) && allocator.partFlags[0] == 129);
        require(allocator.requestNoteRelease(0,60,0,retained) == true);
        require(allocator.requestNoteRelease(0,61,0,retained) == true);
        require(allocator.allocations[first->voices[0]].releaseCommand == 0 && allocator.allocations[second->voices[0]].releaseCommand == 0);
        retained[0] = 61;
        require(allocator.setPartHold(0,false,retained) && allocator.partFlags[0] == 128);
        require(allocator.noteGroups[first->group].status == 2 && allocator.noteGroups[second->group].status == 2);
        require(allocator.allocations[first->voices[0]].releaseCommand == 255 && allocator.allocations[first->voices[1]].releaseCommand == 255);
        require(allocator.allocations[second->voices[0]].releaseCommand == 0 && allocator.noteGroups[second->group].retirementFlags == 0);
        const auto before = SnapshotAllocationField(allocator,&sc55::VoiceAllocator::VoiceAllocation::releaseCommand);
        retained.fill(255);
        require(allocator.setPartHold(0,false,retained) && SnapshotAllocationField(allocator,&sc55::VoiceAllocator::VoiceAllocation::releaseCommand) == before);
        // Failure late in the chain must not commit earlier group/part clears.
        require(allocator.setPartHold(0,true,retained));
        allocator.noteGroups[first->group].retirementFlags = allocator.noteGroups[second->group].retirementFlags = 1;
        allocator.groups.tail[second->group] = 24;
        require(!allocator.setPartHold(0,false,retained));
        require(allocator.partFlags[0] == 129 && allocator.noteGroups[first->group].retirementFlags == 1
            && allocator.noteGroups[second->group].retirementFlags == 1 && SnapshotAllocationField(allocator,&sc55::VoiceAllocator::VoiceAllocation::releaseCommand) == before);
        allocator.noteGroups[second->group].next = first->group;
        for(auto& group:allocator.noteGroups) group.retirementFlags=0;
        require(!allocator.setPartHold(0,false,retained));
        require(!allocator.setPartHold(16,true,retained));
    }
    {
        sc55::VoiceAllocator allocator; require(allocator.initializeTables());
        const auto first = allocator.createGroup({0,3,60,1,2});
        const auto second = allocator.createGroup({0,3,60,1,2});
        require(first && second);
        std::array<uint8_t,16> retained; retained.fill(255);
        const sc55::MidiDecoder::Event off{sc55::MidiDecoder::Kind::message,0x80,60,64,2};
        require(sc55::RequestMelodicNoteOff(off,0,2,retained,allocator) == false);
        require(sc55::RequestMelodicNoteOff(off,0,3,retained,allocator) == true);
        require(allocator.noteGroups[first->group].status == 2 && allocator.noteGroups[second->group].status == 0);
        for (auto slot : {first->voices[0],first->voices[1]})
            require(allocator.allocations[slot].releaseRequested == 1 && allocator.allocations[slot].releaseCommand == 255);
        require(allocator.allocations[second->voices[0]].releaseCommand == 0);
        auto zeroOn = off; zeroOn.status = 0x90; zeroOn.second = 0;
        require(sc55::RequestMelodicNoteOff(zeroOn,0,0,retained,allocator) == true);
        require(sc55::RequestMelodicNoteOff(off,0,0,retained,allocator) == false);
        zeroOn.second = 1;
        require(!sc55::RequestMelodicNoteOff(zeroOn,0,0,retained,allocator).has_value());
        const auto deferred = allocator.createGroup({1,0,60,1,1}); require(bool(deferred));
        allocator.partFlags[1] = 1; allocator.noteGroups[deferred->group].retirementFlags = 128;
        require(allocator.requestNoteRelease(1,60,0,retained) == true);
        require(allocator.noteGroups[deferred->group].status == 2 && allocator.noteGroups[deferred->group].retirementFlags == 129
            && allocator.allocations[deferred->voices[0]].releaseCommand == 0);
        const auto held = allocator.createGroup({2,0,60,1,1}); require(bool(held));
        retained[0] = 60;
        require(allocator.requestNoteRelease(2,60,0,retained) == true);
        require(allocator.noteGroups[held->group].status == 2 && allocator.allocations[held->voices[0]].releaseCommand == 0);
        const auto ended = allocator.createGroup({3,0,60,1,1}); require(bool(ended));
        retained[0] = 255; retained[1] = 60;
        require(allocator.requestNoteRelease(3,60,0,retained) == true);
        require(allocator.allocations[ended->voices[0]].releaseCommand == 255);
        const auto corrupt = allocator.createGroup({4,0,60,1,1}); require(bool(corrupt));
        allocator.groups.tail[corrupt->group] = 24;
        const auto flags = SnapshotAllocationField(allocator,&sc55::VoiceAllocator::VoiceAllocation::releaseCommand);
        require(!allocator.requestNoteRelease(4,60,0,retained).has_value());
        require(allocator.noteGroups[corrupt->group].status == 0 && SnapshotAllocationField(allocator,&sc55::VoiceAllocator::VoiceAllocation::releaseCommand) == flags);
        allocator.noteGroups[corrupt->group].next = corrupt->group;
        require(!allocator.requestNoteRelease(4,61,0,retained).has_value());
        require(!allocator.requestNoteRelease(16,60,0,retained).has_value());
    }
    {
        sc55::VoiceAllocator allocator;
        require(allocator.initializeTables());
        const auto allocation = allocator.createGroup({0,0,60,0,1});
        require(allocation && allocator.freeCount == 23);
        require(allocator.returnVoice(allocation->voices[0]));
        require(allocator.freeCount == 23); // Reserved is not yet active.
        allocator.allocations[allocation->voices[0]].status = 0; // 113a, separate from createGroup.
        require(allocator.returnVoice(allocation->voices[0]));
        require(allocator.freeCount == 24);
    }
    {
        sc55::VoiceAllocator allocator;
        std::array<sc55::VoiceReleaseAuxiliary,24> voices{};
        for (unsigned slot = 0; slot < 24; ++slot)
        {
            allocator.allocations[slot].releaseCommand = uint8_t(slot*11);
            voices[slot].pending = 255; voices[slot].activity = 17;
        }
        const auto flags = SnapshotAllocationField(allocator,&sc55::VoiceAllocator::VoiceAllocation::releaseCommand);
        unsigned remaining = 24;
        allocator.publishReleaseRequests([&](uint8_t slot,uint8_t request) {
            require(slot == --remaining); voices[slot].pending = request;
        });
        require(remaining == 0 && SnapshotAllocationField(allocator,&sc55::VoiceAllocator::VoiceAllocation::releaseCommand) == flags);
        for (unsigned slot = 0; slot < 24; ++slot)
            require(voices[slot].pending == flags[slot] && voices[slot].activity == 17);
        for(auto& voice:allocator.allocations) voice.releaseCommand=0;
        allocator.publishReleaseRequests([&](uint8_t slot,uint8_t request) { voices[slot].pending = request; });
        for (const auto& voice : voices) require(voice.pending == 0);
    }
    {
        sc55::ControlTaskClock clock;
        constexpr uint64_t period = 160256;
        require(!clock.consume());
        clock.advance(period-1); require(!clock.consume() && clock.untilNextExpiration() == 1);
        clock.advance(1); require(clock.consume() == 1 && !clock.consume());
        clock.advance(period*5); require(clock.consume() == 5);
        clock.advance(period*256); require(clock.consume() == 0 && !clock.consume());
        clock.advance(period*257+13); require(clock.consume() == 1 && clock.untilNextExpiration() == period-13);
        require(!clock.reset(0) && clock.untilNextExpiration() == period-13);
        sc55::ControlTaskClock partitioned,whole;
        for (unsigned i = 0; i < 10000; ++i) partitioned.advance(i%129);
        uint64_t total = 0; for (unsigned i = 0; i < 10000; ++i) total += i%129;
        whole.advance(total);
        require(partitioned.consume() == whole.consume() && partitioned.untilNextExpiration() == whole.untilNextExpiration());
    }
    {
        sc55::PanTable table{};
        for (unsigned i = 0; i < table.size(); ++i) table[i] = uint8_t(i);
        sc55::SpatialState initial;
        require(initial.initializePan(64,false,table) && initial.panWord == 0x4040);
        require(initial.advance({},table) && initial.panWord == 0x4040);
        require(initial.initializePan(128,true,table) && initial.pan == 65535 && initial.panWord == 0x0080);
        require(!initial.initializePan(129,false,table) && initial.pan == 65535 && initial.panWord == 0x0080);
        sc55::SpatialInputs start; start.pan = 0; start.reverb = 100; start.chorus = 80;
        unsigned initialIo = 0;
        const auto initialRead = [&](uint8_t a)->uint8_t { ++initialIo; return a == 0x3a ? 170 : 85; };
        const auto initialWrite = [&](uint8_t a,uint8_t v) { ++initialIo; require(a == 0x3e && v == 30); };
        require(initial.initialize(start,table,initialRead,initialWrite) && initialIo == 4
            && initial.pan == 65535 && initial.panWord == 0x2b55 && initial.effects == 0x5064);
        start.pan = 128; initialIo = 0;
        require(!initial.initialize(start,table,initialRead,initialWrite) && initialIo == 0
            && initial.panWord == 0x2b55 && initial.effects == 0x5064);
        sc55::SpatialInputs input; input.pan = 127; input.reverb = 100; input.chorus = 0;
        sc55::SpatialState state{64,0x1234,0x6464};
        require(state.advance(input,table));
        require(state.pan == 65 && state.panWord == 0x3f41 && state.effects == 0x6364);
        state.pan = 65535; const auto held = state.panWord;
        require(state.advance(input,table) && state.pan == 65535 && state.panWord == held && state.effects == 0x6264);
        state.pan = 64; input.pan = 128; const auto effects = state.effects;
        require(!state.advance(input,table) && state.pan == 64 && state.effects == effects);
        sc55::VoiceOutputState output; const sc55::LevelInputs level{};
        require(output.advance(2,level,input,table) == sc55::VoiceOutputState::Result::invalidInput && output.tva.ramp == 0);
        require(output.advance(14,level,input,table) == sc55::VoiceOutputState::Result::stopped && output.tva.ramp == 0);
        input.pan = 64; input.masterPan = 0;
        require(output.advance(2,level,input,table) == sc55::VoiceOutputState::Result::updated && output.tva.ramp == 8192);
        sc55::VoiceStopState voice; voice.stages[0] = 2;
        sc55::PreparedVoicePcm prepared; prepared.pcm12 = 0xdead; prepared.pcm14 = 0xbeef;
        output.spatial.panWord = 0x3456; output.spatial.effects = 0x789a;
        const sc55::SecondEnvelopePcmState second{};
        std::array<uint8_t,64> registers{};
        require(sc55::UpdateVoicePcm(0,voice,prepared,output,second,
            [&](uint8_t a,uint8_t value) { registers[a] = value; }) == sc55::VoicePcmUpdateResult::written);
        require(registers[0x12] == 0x34 && registers[0x13] == 0x56 && registers[0x14] == 0x78 && registers[0x15] == 0x9a);
        require(prepared.pcm12 == 0xdead && prepared.pcm14 == 0xbeef);
    }
    {
        sc55::LevelInputs input; input.expression = input.velocity = input.master = 100;
        sc55::TvaState tva;
        const auto target = sc55::ComputeLevel(input);
        require(target != 0);
        sc55::TvaState initialized{123,456,789};
        initialized.initialize(input);
        require(initialized.ramp == 65535 && initialized.level == target
            && initialized.command == uint16_t((target&0xff00)|0xba));
        sc55::VoiceStopState start;
        start.flagMinus3B = 128; start.cached18 = 0x1234;
        sc55::PrepareVoiceTva(start,initialized);
        require(start.cached16 == initialized.command && start.cached18 == 0x1234);
        std::array<uint8_t,64> initialRegisters{};
        require(sc55::CommitPreparedVoice(0,start,{},[&](uint8_t a,uint8_t value) { initialRegisters[a] = value; }));
        require(uint16_t((initialRegisters[0x16]<<8)|initialRegisters[0x17]) == initialized.command);
        require(initialized.advance(2,input) && initialized.ramp == 65535
            && initialized.level == uint16_t(target-1) && initialized.command == 0xff00);
        initialized.initialize({});
        require(initialized.ramp == 65535 && initialized.level == 0 && initialized.command == 0x00ba);
        for (unsigned step = 0; step < 8; ++step)
        {
            require(tva.advance(2,input));
            require(tva.ramp == (step == 7 ? 65535 : (step+1)*8192));
        }
        require(tva.level == uint16_t(target-1));
        require(tva.advance(12,input) && tva.command == 0xff00);
        const auto previous = tva;
        require(!tva.advance(13,input) && tva.level == previous.level && tva.ramp == previous.ramp && tva.command == previous.command);
        tva.level = 0; // simulated PCM readback must become the comparison baseline
        require(tva.advance(2,input) && uint8_t(tva.command) == 0xb4);
        sc55::VoiceStopState voice; voice.stages[0] = 2; voice.cached16 = 0xdead;
        const sc55::PreparedVoicePcm prepared{};
        const sc55::SecondEnvelopePcmState second{};
        uint16_t written = 0; unsigned writes = 0;
        const auto write = [&](uint8_t a,uint8_t value) {
            ++writes;
            if (a == 0x16) written = uint16_t(value<<8);
            if (a == 0x17) written |= value;
        };
        require(sc55::UpdateVoicePcm(0,voice,prepared,tva,second,write) == sc55::VoicePcmUpdateResult::written);
        require(written == tva.command && voice.cached16 == 0xdead && writes == 15);
        voice.stages[0] = 14; writes = 0;
        require(sc55::UpdateVoicePcm(0,voice,prepared,tva,second,write) == sc55::VoicePcmUpdateResult::idle && writes == 0);
    }
    {
        int32_t level = 0;
        sc55::ApplyModulation(level,-1,0,32767);
        require(level == 0);
        level = 50000; sc55::ApplyModulation(level,-32768,-32768,12345);
        require(level == 50000);
        level = 50000; sc55::ApplyModulation(level,0,-32768,32767);
        require(level == 17233);
        level = 65535; sc55::ApplyModulation(level,1,0,32767);
        require(level == 0);
    }
    {
        std::array<sc55::FirstModulationVoice,24> voices{};
        const std::array<uint16_t,128> depths{};
        const std::array<uint16_t,256> rates{};
        const sc55::LfoWaveformTables tables;
        sc55::FirstVoiceModulationUpdate task;
        using Result = sc55::FirstVoiceModulationUpdate::Result;
        voices[0].sharing = {1,44,255}; voices[1].sharing = {2,55,1};
        voices[0].block.delayRate = 123; voices[0].block.waveform = 3;
        voices[1].block.delayRate = 999; voices[1].block.waveform = 2; voices[1].block.wave.phase = 54321;
        require(task.begin(0,voices,0,64,depths) == Result::shared);
        require(voices[0].block.wave.phase == 54321 && voices[0].block.delayRate == 123
            && voices[0].block.waveform == 3 && voices[0].sharing.source == 1 && voices[0].sharing.baseRate == 44);
        voices[1].firstStage = 14; voices[2].sharing.source = 1;
        require(task.begin(0,voices,0,64,depths) == Result::ready);
        require(voices[0].sharing.source == 24 && voices[2].sharing.source == 0);
        require(task.begin(0,voices,0,64,depths) == Result::invalidInput);
        ++voices[0].firstStage;
        unsigned io = 0;
        const auto read = [&](uint8_t)->uint8_t { ++io; return 0; };
        const auto write = [&](uint8_t,uint8_t) { ++io; };
        require(task.resume(voices,3,0,64,64,depths,rates,tables,read,write) == Result::stageChanged);
        require(voices[0].block.wave.phase == 54321 && io == 0);
        require(task.resume(voices,3,0,64,64,depths,rates,tables,read,write) == Result::invalidInput);
        require(task.begin(0,voices,0,64,depths) == Result::ready);
        ++voices[0].firstStage;
        require(task.resume(voices,3,0,64,64,depths,rates,tables,read,write) == Result::updated);
        require(voices[0].block.rateIndex == 44 && io == 0);
        voices[0].sharing.sharing = 1;
        require(task.begin(0,voices,0,64,depths) == Result::invalidInput);
    }
    {
        std::array<sc55::VoiceModulation,24> voices{};
        std::array<uint8_t,24> sources{}; sources.fill(24);
        SC55Partial partial{}; partial.raw[4] = 16; partial.raw[5] = 33;
        const std::array<uint16_t,256> table{};
        const sc55::LfoWaveformTables waveforms;
        unsigned reads = 0,writes = 0;
        const auto readSeed = [&](uint8_t a)->uint8_t { ++reads; return a == 0x3a ? 0x12 : 0x34; };
        const auto writeSeed = [&](uint8_t,uint8_t) { ++writes; };
        voices[0].block.depth = {11,22,33};
        voices[23].block.depth = {44,55,66}; voices[23].block.output = {77,88,99};
        voices[23].block.rateIndex = 254; voices[23].block.wave.phase = 12345;
        require(sc55::InitializeSecondVoiceModulation(0,voices,sources,partial,table,table,waveforms,readSeed,writeSeed)
            == sc55::ModulationRoute::shared);
        require(sources[0] == 23 && voices[0].sharing == 255 && reads == 0 && writes == 0);
        require(voices[0].block.depth == std::array<uint16_t,3>{11,22,33}
            && voices[0].block.output == std::array<uint16_t,3>{77,88,99}
            && voices[0].block.rateIndex == 254 && voices[0].block.wave.phase == 12345);
        partial.raw[4] = 0;
        require(sc55::InitializeSecondVoiceModulation(0,voices,sources,partial,table,table,waveforms,readSeed,writeSeed)
            == sc55::ModulationRoute::local);
        require(sources[0] == 24 && voices[0].sharing == 0 && reads == 3 && writes == 1
            && voices[0].block.rateIndex == 33 && voices[0].block.wave.held == 0x1234);
        require(sc55::InitializeSecondVoiceModulation(24,voices,sources,partial,table,table,waveforms,readSeed,writeSeed)
            == sc55::ModulationRoute::invalidInput);
        require(reads == 3 && writes == 1 && sources[0] == 24);
        sc55::VoiceStopState stop;
        partial.raw[8] = 199; voices[0].fieldA2 = 77;
        const auto phase = voices[0].block.wave.phase;
        for (unsigned flag = 0; flag < 128; ++flag)
        {
            stop.flagMinus3B = uint8_t(flag);
            require(sc55::PrepareVoiceSecondModulation(0,stop,voices,sources,partial,table,table,waveforms,readSeed,writeSeed)
                == sc55::SecondModulationPreparation::unchanged);
            require(voices[0].fieldA2 == 77 && voices[0].block.wave.phase == phase && sources[0] == 24);
        }
        require(reads == 3 && writes == 1);
        stop.flagMinus3B = 128;
        require(sc55::PrepareVoiceSecondModulation(0,stop,voices,sources,partial,table,table,waveforms,readSeed,writeSeed)
            == sc55::SecondModulationPreparation::local);
        require(voices[0].fieldA2 == 199 && stop.flagMinus3B == 128 && reads == 6 && writes == 2);
    }
    {
        std::array<sc55::FirstModulationVoice,24> voices{};
        sc55::FirstModulationInputs input; input.mode = 16; input.baseRate = 7;
        std::array<uint16_t,256> timing{},rates{};
        std::array<uint16_t,128> depths{};
        const sc55::LfoWaveformTables tables;
        unsigned reads = 0,writes = 0;
        const auto sourceRead = [&](uint8_t a)->uint8_t { ++reads; return a == 0x3a ? 0x12 : 0x34; };
        const auto sourceWrite = [&](uint8_t,uint8_t) { ++writes; };
        voices[23].block.wave.phase = 111; voices[22].block.wave.phase = 222;
        voices[23].sharing = {22,33,255}; voices[22].sharing = {21,44,1};
        require(sc55::InitializeFirstVoiceModulation(0,voices,input,timing,depths,rates,tables,sourceRead,sourceWrite)
            == sc55::ModulationRoute::shared);
        require(voices[0].block.wave.phase == 111 && voices[0].sharing.source == 22 && reads == 0 && writes == 0);
        require(sc55::InitializeFirstVoiceModulation(23,voices,input,timing,depths,rates,tables,sourceRead,sourceWrite)
            == sc55::ModulationRoute::shared);
        require(voices[23].block.wave.phase == 222 && voices[23].sharing.baseRate == 44);
        input.mode = 0;
        require(sc55::InitializeFirstVoiceModulation(0,voices,input,timing,depths,rates,tables,sourceRead,sourceWrite)
            == sc55::ModulationRoute::local);
        require(reads == 3 && writes == 1 && voices[0].sharing.source == 24 && voices[0].sharing.sharing == 0
            && voices[0].sharing.baseRate == 7 && voices[0].block.wave.held == 0x1234);
        input.mode = 16; voices[23].sharing.source = 25;
        require(sc55::InitializeFirstVoiceModulation(0,voices,input,timing,depths,rates,tables,sourceRead,sourceWrite)
            == sc55::ModulationRoute::invalidInput);
        require(reads == 3 && writes == 1 && voices[0].sharing.source == 24);
        require(sc55::SelectFirstModulationSource(24,16,voices) == 25);
    }
    {
        sc55::ModulationBlock block,source;
        std::array<uint16_t,128> depths{}; depths[1] = 32767;
        block.depth = {32767,32768,123}; block.rateIndex = 55;
        source.delay = source.attack = 65535; source.wave.phase = 456;
        source.rateIndex = 99; source.depth = {1,2,3};
        require(sc55::InitializeSharedFirstModulationBlock(block,source,1,64,depths));
        require(block.output[0] == 32767 && block.output[1] == 32768 && block.output[2] == 32766);
        require(block.depth[2] == 32767 && block.rateIndex == 55 && block.wave.phase == 456);
        require(sc55::InitializeSharedFirstModulationBlock(block,source,129,64,depths));
        require(block.output[2] == uint16_t(0u-32766));
        source.delay = 65534;
        require(sc55::InitializeSharedFirstModulationBlock(block,source,1,64,depths));
        require(block.output == std::array<uint16_t,3>{} && block.depth[2] == uint16_t(0u-32767));
        source.wave.phase = 999;
        require(!sc55::InitializeSharedFirstModulationBlock(block,source,1,128,depths));
        require(block.wave.phase == 456 && block.delay == 65534);
        block.delay = block.attack = 65535;
        require(sc55::InitializeSharedFirstModulationBlock(block,block,1,64,depths));
        require(block.wave.phase == 456 && block.output[2] == 32766);
        sc55::FirstModulationSharing sharing{24,0,0},sourceSharing{3,99,1};
        require(sc55::InitializeSharedFirstModulation(block,sharing,source,sourceSharing,1,64,depths));
        require(sharing.source == 3 && sharing.baseRate == 99 && sharing.sharing == 1);
        sourceSharing.source = 25; source.wave.phase = 1000;
        require(!sc55::InitializeSharedFirstModulation(block,sharing,source,sourceSharing,1,64,depths));
        require(sharing.source == 3 && block.wave.phase == 999);
        require(sc55::InitializeSharedFirstModulation(block,sharing,block,sharing,1,64,depths));
        require(sharing.source == 3 && block.wave.phase == 999);
    }
    auto pcm = std::make_unique<pcm_t>();
    mcu->is_mk1 = true;
    PCM_Init(*pcm,*mcu);
    using Runner = sc55::EnvelopeRunner;
    using Stage = sc55::EnvelopeStage;
    const Runner::Setup setup{{{{{0,0},{1,0},{2,0},{3,0},{4,0}}},256,256},{100,80,60,40},256,256,0};
    const Runner::State initial{{Stage::attack1,{0,0},{0,0},0,100},0,0xff00,65535};
    const auto read = [&](uint8_t address) { return PCM_Read(*pcm,address); };
    const auto write = [&](uint8_t address,uint8_t value) { PCM_Write(*pcm,address,value); };
    const auto word = [&](uint8_t address) {
        (void)read(address); const auto hi = read(0x3a); const auto lo = read(0x3b);
        return uint16_t((hi<<8)|lo);
    };
    unsigned syncChecks = 0;
    {
        sc55::ModulationDepthTables tables;
        tables.secondEnvelope[3] = 1234; tables.pitch[4] = 5678;
        SC55Partial partial{};
        partial.raw[0x48] = 128; partial.raw[0x49] = 255;
        partial.raw[0x2a] = 3; partial.raw[0x2b] = 131;
        partial.raw[0x0f] = 132; partial.raw[0x0e] = 77;
        sc55::ModulationBlock first,second;
        first.depth[2] = 999; first.wave.phase = 123; second.output = {1,2,3};
        sc55::FirstModulationInputs input;
        input.pitchDepth = sc55::PrepareModulationDepths(partial,first,second,tables);
        require(first.depth == std::array<uint16_t,3>{0,1234,999});
        require(second.depth == std::array<uint16_t,3>{uint16_t(-32512),uint16_t(-1234),uint16_t(-5678)});
        require(input.pitchDepth == 77 && first.wave.phase == 123 && second.output == std::array<uint16_t,3>{1,2,3});
        tables.pitch[77] = 4321;
        require(sc55::PrepareFirstModulationControls(first,0,input.pitchDepth,64,64,tables.pitch));
        require(first.depth[2] == 4321 && first.depth[1] == 1234 && first.wave.phase == 123);
    }
    {
        sc55::ModulationBlock block;
        block.depth = {11,22,33}; block.wave.phase = 123;
        sc55::FirstModulationInputs input;
        std::array<uint16_t,256> rates{},timing{};
        std::array<uint16_t,128> depths{};
        const sc55::LfoWaveformTables tables;
        unsigned reads = 0, writes = 0;
        const auto sourceRead = [&](uint8_t address)->uint8_t {
            constexpr std::array<uint8_t,3> addresses{0x34,0x3a,0x3b};
            require(writes == 1 && reads < 3 && address == addresses[reads]);
            return std::array<uint8_t,3>{0,0x12,0x34}[reads++];
        };
        const auto sourceWrite = [&](uint8_t a,uint8_t v) { require(reads == 0 && writes++ == 0 && a == 0x3e && v == 30); };
        input.depthControl = 128;
        require(!sc55::InitializeFirstModulation(block,input,timing,depths,rates,tables,sourceRead,sourceWrite));
        require(reads == 0 && writes == 0 && block.wave.phase == 123 && block.depth[2] == 33);
        input.depthControl = 64;
        require(sc55::InitializeFirstModulation(block,input,timing,depths,rates,tables,sourceRead,sourceWrite));
        require(reads == 3 && writes == 1 && block.wave.held == 0x1234 && block.wave.smoothed == 0x1234
            && block.wave.phase == 0 && block.depth[0] == 11 && block.depth[1] == 22 && block.depth[2] == 0);
        reads = writes = 0;
        SC55Partial partial{};
        partial.raw[4] = 0xc2; partial.raw[5] = 255; partial.raw[6] = 128; partial.raw[7] = 255;
        timing[255] = 6789; rates[255] = 100;
        block.depth = {123,456,789}; block.rateModifier = 0;
        sc55::InitializeSecondModulation(block,partial,timing,rates,tables,sourceRead,sourceWrite);
        require(reads == 3 && writes == 1 && block.waveform == 2 && block.wave.phase == 0xc064
            && block.rateIndex == 255 && block.delayRate == 0 && block.attackRate == 6789
            && block.depth == std::array<uint16_t,3>{123,456,789} && block.delay == 0 && block.attack == 0);
    }
    {
        std::array<uint16_t,128> depths{};
        for (unsigned i = 0; i < depths.size(); ++i) depths[i] = uint16_t(i*100);
        sc55::ModulationBlock block;
        block.depth = {11,22,33}; block.wave.phase = 1234;
        require(sc55::PrepareFirstModulationControls(block,60,20,70,69,depths));
        require(block.rateIndex == 66 && block.depth[2] == 3000);
        require(sc55::PrepareFirstModulationControls(block,60,148,70,69,depths));
        require(block.depth[2] == uint16_t(-1000));
        require(block.depth[0] == 11 && block.depth[1] == 22 && block.wave.phase == 1234);
        require(!sc55::PrepareFirstModulationControls(block,0,0,128,64,depths) && block.rateIndex == 66);
        require(!sc55::PrepareFirstModulationControls(block,0,0,64,128,depths) && block.depth[2] == uint16_t(-1000));
        block.delay = block.attack = 65535; block.waveform = 2;
        std::array<uint16_t,256> rates{}; rates[66] = 123;
        const sc55::LfoWaveformTables tables;
        require(block.advance(1,rates,tables,[](uint8_t)->uint8_t { throw std::runtime_error("Unexpected controller test PCM read"); },
            [](uint8_t,uint8_t) { throw std::runtime_error("Unexpected controller test PCM write"); }));
        require(block.wave.phase == 1357 && block.output[2] == uint16_t(-1000));
    }
    {
        std::array<uint16_t,256> timing{};
        for (unsigned i = 0; i < timing.size(); ++i) timing[i] = uint16_t(i+1);
        sc55::ModulationBlock block;
        block.depth = {1,2,3}; block.wave.held = 1234;
        block.rateIndex = 77; block.rateModifier = -123;
        require(sc55::PrepareFirstModulation(block,0xca,0,255,0,timing));
        require(block.waveform == 6 && block.wave.phase == 0xc000
            && block.delayRate == 1 && block.attackRate == 256 && block.wave.held == 1234
            && block.depth == std::array<uint16_t,3>{1,2,3} && block.rateIndex == 77 && block.rateModifier == -123);
        require(sc55::PrepareFirstModulation(block,0,127,0,127,timing) && block.delayRate == 128);
        require(sc55::PrepareFirstModulation(block,0,128,0,64,timing) && block.delayRate == 0);
        block.delay = 999;
        require(!sc55::PrepareFirstModulation(block,0,0,0,128,timing) && block.delay == 999);
    }
    {
        sc55::ModulationBlock first,second;
        first.output = {11,22,33}; first.wave.output = 44;
        second.output = {55,66,77}; second.wave.output = 88;
        sc55::SecondEnvelopeOutputInputs envelope;
        envelope.base = 99; envelope.control = 100; envelope.offset = 101;
        envelope.suppressPositiveControl = true;
        envelope.sources[0].second = 102; envelope.sources[1].second = 103;
        sc55::PitchModulationInputs pitch;
        pitch.offset = 104; pitch.masterTune = 105; pitch.partTune = 106;
        pitch.sources[0].second = 107; pitch.sources[1].second = 108;
        sc55::LevelInputs level;
        level.expression = 109; level.velocity = 110; level.master = 111;
        level.tone_scale = 112; level.has_tone_scale = true; level.bias = -113;
        level.mod1_b = -114; level.mod2_b = 115;
        sc55::ApplyVoiceModulationOutputs(first,second,level,envelope,pitch);
        require(level.mod1_a == 11 && level.mod2_a == 55 && level.mod1_depth == 44 && level.mod2_depth == 88);
        require(level.expression == 109 && level.velocity == 110 && level.master == 111
            && level.tone_scale == 112 && level.has_tone_scale && level.bias == -113
            && level.mod1_b == -114 && level.mod2_b == 115);
        require(envelope.sources[0].first == 22 && envelope.sources[1].first == 66
            && pitch.sources[0].first == 33 && pitch.sources[1].first == 77);
        require(envelope.sources[0].waveform == 44 && envelope.sources[1].waveform == 88
            && pitch.sources[0].waveform == 44 && pitch.sources[1].waveform == 88);
        require(envelope.base == 99 && envelope.control == 100 && envelope.offset == 101
            && envelope.suppressPositiveControl && envelope.sources[0].second == 102 && envelope.sources[1].second == 103);
        require(pitch.offset == 104 && pitch.masterTune == 105 && pitch.partTune == 106
            && pitch.sources[0].second == 107 && pitch.sources[1].second == 108);
        first.wave.output = 0xffff;
        first.output[0] = 0x8000;
        sc55::ApplyVoiceModulationOutputs(first,second,level,envelope,pitch);
        require(level.mod1_depth == -1 && level.mod1_a == -32768 && level.mod1_b == -114);
        require(envelope.sources[0].waveform == 0xffff && pitch.sources[0].waveform == 0xffff);
    }
    {
        std::array<sc55::VoiceModulation,24> voices{};
        std::array<uint8_t,24> sources{};
        sources.fill(24); sources[0] = 1; sources[2] = 1; sources[3] = 4;
        voices[0].sharing = 1; voices[0].block.depth = {1,2,3};
        voices[1].block.output = {11,22,33}; voices[1].block.wave.phase = 1234;
        require(sc55::RouteVoiceModulation(0,voices,sources) == sc55::ModulationRoute::shared);
        require(voices[0].block.output == voices[1].block.output && voices[0].block.wave.phase == 1234
            && voices[0].block.depth == std::array<uint16_t,3>{1,2,3});
        voices[1].firstStage = 22;
        require(sc55::RouteVoiceModulation(0,voices,sources) == sc55::ModulationRoute::local);
        require(sources[0] == 24 && sources[2] == 0 && sources[3] == 4 && voices[0].sharing == 0);
        voices[0].sharing = 1;
        require(sc55::RouteVoiceModulation(0,voices,sources) == sc55::ModulationRoute::invalidInput && voices[0].sharing == 1);
        require(sc55::RouteVoiceModulation(24,voices,sources) == sc55::ModulationRoute::invalidInput);
        const sc55::LfoWaveformTables tables;
        std::array<uint16_t,256> rates{}; rates[0] = 100;
        sc55::VoiceModulationUpdate task;
        using Result = sc55::VoiceModulationUpdate::Result;
        const auto noRead = [&](uint8_t)->uint8_t { require(false); return 0; };
        const auto noWrite = [&](uint8_t,uint8_t) { require(false); };
        sources[0] = 1;
        require(task.begin(0,voices,sources) == Result::ready);
        require(task.begin(2,voices,sources) == Result::invalidInput);
        ++voices[0].firstStage;
        require(task.resume(voices,1,rates,tables,noRead,noWrite) == Result::stageChanged);
        require(voices[0].block.wave.phase == 1234);
        require(task.resume(voices,1,rates,tables,noRead,noWrite) == Result::invalidInput);
        require(task.begin(0,voices,sources) == Result::ready);
        ++voices[0].firstStage; // direct local path has no stage gate
        require(task.resume(voices,1,rates,tables,noRead,noWrite) == Result::updated);
        require(voices[0].block.wave.phase == 1334);
    }
    {
        const sc55::LfoWaveformTables tables;
        sc55::LfoWaveformState lfo{65535,123,0,456};
        unsigned reads = 0, writes = 0;
        const auto sourceRead = [&](uint8_t address) -> uint8_t {
            constexpr std::array<uint8_t,3> addresses{0x34,0x3a,0x3b};
            require(writes == 1 && reads < 3 && address == addresses[reads]);
            return std::array<uint8_t,3>{0,0x81,0x23}[reads++];
        };
        const auto sourceWrite = [&](uint8_t a,uint8_t v) {
            require(reads == 0 && writes++ == 0 && a == 0x3e && v == 30);
        };
        require(!lfo.advance(7,1,tables,sourceRead,sourceWrite));
        require(lfo.phase == 65535 && lfo.output == 456 && reads == 0 && writes == 0);
        require(lfo.advance(4,0,tables,sourceRead,sourceWrite));
        require(lfo.phase == 65535 && lfo.output == 123 && reads == 0 && writes == 0);
        require(lfo.advance(4,1,tables,sourceRead,sourceWrite));
        require(lfo.phase == 1 && lfo.held == 0x8123 && lfo.output == 0x8123 && reads == 3 && writes == 1);
        const auto noRead = [&](uint8_t)->uint8_t { require(false); return 0; };
        const auto noWrite = [&](uint8_t,uint8_t) { require(false); };
        for (uint8_t waveform : {uint8_t(5),uint8_t(6)})
        {
            lfo = {0,32767,32760,0};
            require(lfo.advance(waveform,0,tables,noRead,noWrite) && lfo.output == 32767);
            lfo = {0,0x8000,0x8005,0};
            require(lfo.advance(waveform,0,tables,noRead,noWrite) && lfo.output == 0x8000);
        }
        std::array<uint16_t,256> rates{};
        rates[0] = 100;
        sc55::ModulationBlock block;
        block.waveform = 2; block.depth = {32767,0x8000,0xffff};
        block.output = {10,20,30}; block.delayRate = 1;
        require(block.advance(1,rates,tables,noRead,noWrite));
        require(block.delay == 1 && block.attack == 0 && block.wave.phase == 100
            && block.output == std::array<uint16_t,3>{10,20,30});
        block.delay = 65534; block.attack = 65534; block.attackRate = 1;
        require(block.advance(1,rates,tables,noRead,noWrite));
        require(block.delay == 65535 && block.attack == 65535
            && block.output == std::array<uint16_t,3>{32766,0x8001,0});
        require(block.advance(0,rates,tables,noRead,noWrite) && block.output == block.depth);
        block.waveform = 7; block.delay = 123;
        require(!block.advance(65535,rates,tables,noRead,noWrite) && block.delay == 123);
    }
    for (unsigned variant = 0; variant < 5; ++variant)
    {
        auto entrySetup = setup;
        entrySetup.delayIncrement = variant == 3 ? 1 : 0;
        auto state = initial;
        state.level = 0x1100; state.pcmWord = 0xb6;
        state.segment.stage = variant == 1 ? Stage::finished
            : variant >= 3 ? Stage::delay : Stage::attack1;
        Runner amplitude(entrySetup,state);
        sc55::VoiceReleaseAuxiliary release;
        release.pending = 1; release.activity = 77;
        release.second.level = 1234; release.second.releaseTarget = 5678;
        sc55::PitchEnvelopeRunner pitch;
        sc55::SecondEnvelopePcmState second;
        second.command = 0xff00;
        sc55::TvaState tva;
        pcm->ram2[0][10] = 0x2345;
        unsigned accesses = 0;
        const auto result = sc55::BeginVoiceUpdate(variant == 0 ? 24 : 0,
            amplitude,release,pitch,tva,second,
            [&](uint8_t a) { ++accesses; return read(a); },
            [&](uint8_t a,uint8_t v) { ++accesses; write(a,v); });
        if (variant < 2)
            require(result == (variant == 0 ? sc55::VoiceUpdateEntry::invalidChannel : sc55::VoiceUpdateEntry::stopped)
                && accesses == 0 && release.pending == 1 && release.activity == 77);
        else if (variant == 2)
            require(result == sc55::VoiceUpdateEntry::update && accesses != 0
                && release.pending == 0 && amplitude.state().level == 0x468a
                && amplitude.state().segment.start == 0x46 && release.activity == 0x46
                && release.second.start == 1234 && release.second.target == 5678);
        else
            require(accesses == 0 && release.pending == 0
                && result == (variant == 3 ? sc55::VoiceUpdateEntry::stopped : sc55::VoiceUpdateEntry::update)
                && amplitude.state().segment.stage == (variant == 3 ? Stage::finished : Stage::attack1));
    }
    {
        unsigned reads = 0, writes = 0;
        constexpr std::array<uint8_t,6> addresses{0x34,0x3a,0x3b,0x34,0x3a,0x3b};
        constexpr std::array<uint8_t,6> values{0,0x80,0x12,0,0x7f,0x34};
        const auto prepared = sc55::PreparePartPitch(60000,65,255,
            [&](uint8_t address) {
                require(writes == 1 && reads < addresses.size() && address == addresses[reads]);
                return values[reads++];
            },
            [&](uint8_t address,uint8_t value) { require(reads == 0 && writes++ == 0 && address == 0x3e && value == 30); });
        require(reads == 6 && writes == 1 && prepared.pitch == 58730 && prepared.cachedRandom == 0x7f);
        pcm->ram2[30][10] = 0x8034;
        const auto real = sc55::PreparePartPitch(60000,65,255,read,write);
        require(real.pitch == 58730 && real.cachedRandom == 0x80 && pcm->select_channel == 30 && pcm->read_latch == 0x8034);
        sc55::PitchGlide glide{{90000,{128,0}},0};
        glide.prepare(32,60,60000,real.pitch);
        require(glide.increment == 1270);
        sc55::PreparedPartPitch state{{60000,1,2},17,{{90000,{128,0}},0}};
        sc55::PartPitchInputs input{60,0,60,1024,1024,1,65,255,32,60};
        const auto forbiddenRead = [&](uint8_t) -> uint8_t { require(false); return 0; };
        const auto forbiddenWrite = [&](uint8_t,uint8_t) { require(false); };
        require(!state.prepare(input,nullptr,forbiddenRead,forbiddenWrite));
        require(state.values.pitch == 60000 && state.values.reference == 1 && state.cachedRandom == 17 && state.glide.pitch.accumulator == 90000);
        sc55::PartPitchKeyTables tables{};
        input.keyTable = 40;
        require(!state.prepare(input,&tables,forbiddenRead,forbiddenWrite));
        input.keyTable = 0;
        require(state.prepare(input,nullptr,read,write));
        require(state.values.pitch == 58730 && state.values.reference == 60000 && state.values.alternateReference == 60000);
        require(state.cachedRandom == 128 && state.glide.increment == 1270);
    }
    {
        const sc55::PitchConversion conversion;
        sc55::PitchEnvelopeSegment segment{{0,0},60000,61000,32768,0};
        require(segment.advance(1) == 60500 && segment.progress.position == 32768);
        require(segment.advance(2) == 60999 && segment.progress.position == 65535 && segment.progress.deferredTicks == 1);
        segment.progress.position = 0; // completed-segment dispatcher action
        segment.retarget(61000,32768);
        require(segment.start == 61000 && segment.target == 61000 && segment.direction == 0 && segment.progress.deferredTicks == 1);
        require(segment.advance(0) == 61000 && segment.progress.position == 32768);
        sc55::PitchEnvelopeSegment down{{0,0},61000,60000,32768,2};
        require(down.advance(1) == 60500);
        sc55::PitchEnvelopeSegment wrapped{{65535,1},0xffffff,0,0,0};
        require(wrapped.advance(65535) == 0xffffff && wrapped.progress.deferredTicks == 0);
        sc55::EnvelopeTimes pitchTimes{};
        pitchTimes[0] = 8; pitchTimes[1] = 9; pitchTimes[2] = 65535;
        require(sc55::PreparePitchEnvelopeIncrement(128,256,256,pitchTimes) == 65535);
        require(sc55::PreparePitchEnvelopeIncrement(129,256,256,pitchTimes) == 58254);
        require(sc55::PreparePitchEnvelopeIncrement(2,65535,65535,pitchTimes) == 8);
        std::array<uint8_t,92> incrementPartial{}; incrementPartial[0x23] = 64; incrementPartial[0x17] = 1;
        require(sc55::PreparePitchEnvelopeFirstIncrement(incrementPartial,127,256,pitchTimes) == 58254);
        incrementPartial[0x23] = 85;
        require(!sc55::PreparePitchEnvelopeFirstIncrement(incrementPartial,127,256,pitchTimes));
        sc55::PitchEnvelopeKeyCurves keyCurves{};
        std::array<uint16_t,256> multipliers;
        for (unsigned i = 0; i < 256; ++i) multipliers[i] = uint16_t(i*2);
        std::array<uint8_t,92> keyPartial{};
        keyPartial[0x1e] = 0xf3; keyPartial[0x1f] = 2; keyPartial[0x20] = 84; keyPartial[0x21] = 44;
        keyCurves.attack[3][60] = 128; keyCurves.release[2][60] = 0;
        const auto pitchKeyScales = sc55::PreparePitchEnvelopeKeyScales(keyPartial,60,keyCurves,multipliers);
        require(pitchKeyScales && (*pitchKeyScales)[0] == 256 && (*pitchKeyScales)[1] == 510);
        keyPartial[0x23] = 64;
        for (unsigned i = 0; i < 5; ++i) keyPartial[0x17+i] = 1;
        const auto timing = sc55::PreparePitchEnvelopeTiming(keyPartial,60,127,keyCurves,multipliers,pitchTimes);
        require(timing && timing->velocityScale == 256);
        for (unsigned i = 0; i < 4; ++i) require(timing->increments[i] == 58254);
        require(timing->increments[4] == 30840);
        keyPartial[0x1f] = 16;
        require(!sc55::PreparePitchEnvelopeKeyScales(keyPartial,60,keyCurves,multipliers));
        require(!sc55::PreparePitchEnvelopeTiming(keyPartial,60,127,keyCurves,multipliers,pitchTimes));
        keyPartial[0x1f] = 2; keyPartial[0x20] = 43;
        require(!sc55::PreparePitchEnvelopeKeyScales(keyPartial,60,keyCurves,multipliers));
        sc55::PitchEnvelopeDepthTables depthTables;
        depthTables.depth[3] = 32768; depthTables.base[1] = 1000; depthTables.velocity[1] = 65535;
        std::array<uint8_t,92> pitchPartial{};
        pitchPartial[0x10] = 3; pitchPartial[0x22] = 63; pitchPartial[0x12] = 65;
        const auto preparedDepth = sc55::PreparePitchEnvelopeDepth(pitchPartial,255,depthTables);
        require(preparedDepth.depth == 373 && preparedDepth.offsets[0] == 255 && preparedDepth.offsets[1] == 64);
        pitchPartial[0x22] = 64;
        const auto neutralDepth = sc55::PreparePitchEnvelopeDepth(pitchPartial,255,depthTables);
        require(neutralDepth.depth == 32768 && neutralDepth.offsets[0] == 1);
        std::array<uint8_t,256> pitchCurve{}; pitchCurve[1] = 255;
        require(sc55::PreparePitchEnvelopeTarget(60000,1,65535,pitchCurve) == 125022);
        require(sc55::PreparePitchEnvelopeTarget(60000,255,65535,pitchCurve) == 0);
        require(sc55::PreparePitchEnvelopeTarget(0xffffff,128,0,pitchCurve) == 0);
        require(sc55::PreparePitchEnvelopeTarget(0xffffff,0,0,pitchCurve) == 0xffffff);
        pitchPartial[0x11] = 255;
        for (unsigned i = 0; i < 5; ++i) pitchPartial[0x12+i] = 64;
        const auto targets = sc55::PreparePitchEnvelopeTargets(60000,pitchPartial,127,128,depthTables,pitchCurve);
        require(targets.pitch[0] == 58720 && targets.pitch[1] == 60000 && targets.pitch[4] == 60000 && targets.direction == 0);
        const auto equalTargets = sc55::PreparePitchEnvelopeTargets(60000,pitchPartial,127,0,depthTables,pitchCurve);
        require(equalTargets.direction == 2 && equalTargets.pitch[0] == 60000);
        const auto base = sc55::PreparePartPitchBase(60,1024,60,1024,1025);
        require(base.pitch == 61024 && base.reference == 60000 && base.alternateReference == 59999);
        require(sc55::PreparePartPitchBase(0,0,0,65535,65535).alternateReference == ((2048u-131070u)&0xffffff));
        require(sc55::ApplyPartPitchKeyCorrection(base.pitch,32778) == 61034);
        require(sc55::ApplyPartPitchKeyCorrection(300,0) == 0);
        require(sc55::ApplyPartPitchFine(60000,64) == 60000);
        require(sc55::ApplyPartPitchFine(300,0) == 0);
        require(sc55::ApplyPartPitchRandom(60000,128,255) == 58720);
        require(sc55::ApplyPartPitchRandom(60000,127,255) == 61270);
        require(sc55::ApplyPartPitchRandom(0xffffff,128,0) == 0);
        require(sc55::ApplyPartPitchRandom(0xffffff,127,0) == 0xffffff);
        sc55::PitchGlideRates rates{}; rates[7] = 30;
        sc55::PitchGlide glide{{81000,{128,0}},100};
        sc55::PitchGlide starting{{90000,{128,0}},17};
        starting.prepare(32,60,80000,81000);
        require(starting.increment == ((60000u-81000u)&0xffffff) && starting.pitch.accumulator == 90000);
        starting.prepare(160,255,80000,81000);
        require(starting.increment == 0 && starting.pitch.accumulator == 90000);
        starting.prepare(0,255,80000,81000);
        require(starting.increment == ((0u-1000u)&0xffffff) && starting.pitch.accumulator == 91000);
        require(!glide.advance(2,128,rates,81000,128,conversion));
        require(glide.increment == 100 && glide.pitch.accumulator == 81000);
        require(glide.advance(2,7,rates,81000,128,conversion).has_value());
        require(glide.increment == 40 && glide.pitch.accumulator == 81040);
        require(glide.advance(2,7,rates,81000,128,conversion).has_value());
        require(glide.increment == 0 && glide.pitch.accumulator == 81040);
        require(glide.advance(2,255,rates,81000,128,conversion).has_value());
        sc55::VoicePitch pitch{0xffffff,{128,0}};
        require(sc55::DecayPitchIncrement(100,2,30) == 40);
        require(sc55::DecayPitchIncrement(0xffff9c,2,30) == 0xffffd8);
        require(sc55::DecayPitchIncrement(100,4,30) == 0);
        require(sc55::DecayPitchIncrement(0xffff9c,4,30) == 0);
        require(sc55::DecayPitchIncrement(100,256,65535) == 356);
        sc55::VoiceStopState pitchVoice;
        pitchVoice.pcm10 = pitch.advance(1,0,128,conversion);
        require(pitch.accumulator == 0 && pitchVoice.pcm10 == 16384);
        require(sc55::CommitPreparedVoice(0,pitchVoice,{},write) && word(0x10) == 16384);
        require(pitch.advance(12000,0,128,conversion) == 32768);
        require(pitch.advance(12000,0,128,conversion) == 65535);
        require(conversion.fromDelta(0) == 32768 && conversion.fromDelta(uint32_t(-12000)) == 16384);
        require(conversion.fromDelta(12000) == 65535 && conversion.correctionDivisor(0x813c68) == 1);
        sc55::PitchCorrectionCache cache{128,123};
        require(cache.update(128,81000,32768,conversion) == 32891 && cache.offset == 123);
        require(cache.update(129,81000,32768,conversion) == 32770 && cache.offset == 2);
        require(cache.update(129,93000,32768,conversion) == 32770 && cache.offset == 2);
        sc55::VoiceStopState voice;
        voice.pcm10 = cache.update(127,81000,32768,conversion);
        require(sc55::CommitPreparedVoice(0,voice,{},write));
        require(word(0x10) == 32766 && cache.offset == 65534);
    }
    require(!sc55::PreparePitchOffset(128,0));
    for (unsigned source = 0; source < 256; ++source)
        for (uint16_t divisor : {uint16_t(1),uint16_t(256),uint16_t(32768),uint16_t(65535)})
        {
            const auto offset = sc55::PreparePitchOffset(uint8_t(source),divisor);
            require(offset.has_value());
            const int32_t raw = (int32_t(source)-128)*65536/int32_t(divisor);
            const int32_t expected = raw < -32767 ? -32767 : raw > 32767 ? 32767 : raw;
            require(*offset == uint16_t(expected));
            sc55::VoiceStopState voice;
            voice.pcm10 = sc55::ApplyPitchOffset(32768,*offset);
            require(sc55::CommitPreparedVoice(0,voice,{},write));
            require(word(0x10) == uint16_t(32768+expected));
        }
    for (uint16_t reference : {uint16_t(0),uint16_t(32768),uint16_t(65535)})
        for (uint16_t offset : {uint16_t(0),uint16_t(1),uint16_t(32767),uint16_t(32768),uint16_t(65535)})
        {
            sc55::VoiceStopState voice;
            const int32_t signedOffset = offset < 32768 ? int32_t(offset) : int32_t(offset)-65536;
            const int32_t sum = int32_t(reference)+signedOffset;
            const uint16_t expected = uint16_t(sum < 0 ? 0 : sum > 65535 ? 65535 : sum);
            voice.pcm10 = sc55::ApplyPitchOffset(reference,offset);
            require(sc55::CommitPreparedVoice(0,voice,{},write));
            require(word(0x10) == expected);
        }
    {
        sc55::VoiceStopState voice;
        sc55::PreparedVoicePcm prepared;
        prepared.pcm12 = 0x1234;
        sc55::VoicePostEnable post{9,0,0};
        sc55::SecondEnvelopeReleaseState segment;
        segment.stage = 8; segment.progress = {123,7}; segment.level = 456;
        sc55::SecondEnvelopeSetup setup;
        setup.mode.pcmMode = 3; setup.activation = {0x2468,0x34ba};
        sc55::SecondEnvelopePcmState second;
        second.control = 65; second.level = 0x5678; second.command = 0x789a;
        sc55::PrepareVoiceSecondEnvelope(voice,prepared,post,segment,setup,second);
        require(voice.stages[1] == 8 && prepared.pcm12 == 0x1234
            && prepared.pcm1c == 0x4103 && prepared.pcm1a == 0x34ba);
        require(post.field65 == 9 && post.level == 0x2468 && post.command == 0x789a);
        voice.flagMinus3B = 128;
        require(sc55::CommitPreparedVoice(0,voice,prepared,write));
        require(word(0x1a) == 0x34ba && word(0x1c) == 0x4103);
        const auto continued = sc55::ContinueVoiceSecondEnvelope(voice,segment);
        require(continued && continued->stage == 0 && continued->level == 456
            && continued->progress.position == 123 && continued->progress.deferredTicks == 7);
        require(second.level == 0x5678 && second.command == 0x789a);
        for (uint16_t invalid : {uint16_t(1),uint16_t(23),uint16_t(65535)})
        {
            voice.stages[1] = invalid;
            require(!sc55::ContinueVoiceSecondEnvelope(voice,segment));
        }
    }
    for (uint8_t flag : {uint8_t(0),uint8_t(128)})
        for (uint16_t delay : {uint16_t(0),uint16_t(65535)})
            for (unsigned stage = 0; stage <= unsigned(Stage::finished); ++stage)
            {
                auto source = initial;
                source.segment.stage = Stage(stage);
                source.segment.progress = {123,456}; source.delayAccumulator = delay;
                Runner prepared(setup,source);
                sc55::VoiceStopState voice;
                voice.stages = {0,20,18}; voice.savedStage = uint16_t(stage == unsigned(Stage::finished) ? 22 : stage*2);
                voice.flagMinus3B = flag;
                sc55::PrepareVoiceAmplitude(voice,prepared);
                require(voice.stages[1] == 20 && voice.stages[2] == 18);
                write(0x3e,0);
                sc55::ActivatePreparedVoice(voice,write);
                const auto continued = sc55::ContinueVoiceAmplitude(voice,prepared);
                require(continued.has_value());
                auto expected = prepared;
                if (flag) expected.activateNewVoice();
                require(continued->state().segment.stage == expected.state().segment.stage
                    && continued->state().segment.progress.position == expected.state().segment.progress.position
                    && continued->state().segment.progress.deferredTicks == 456
                    && continued->state().level == source.level
                    && continued->state().pcmWord == expected.state().pcmWord
                    && continued->state().pcmWord == word(0x18));
            }
    for (uint16_t code : {uint16_t(1),uint16_t(14),uint16_t(18),uint16_t(20),uint16_t(65535)})
    {
        sc55::VoiceStopState voice; voice.stages[0] = code;
        require(!sc55::ContinueVoiceAmplitude(voice,Runner(setup,initial)));
    }
    {
        sc55::SampleBank samples;
        sc55::SampleBank::Group group{0,{}};
        for (unsigned i = 0; i < 16; ++i) group.data[12+i] = 127;
        // Synthetic owned descriptor: nonzero base and carry across 16 bits.
        sc55::SampleBank::Sample sample{0,{0,0x12,0xff,0,2,0,4,0,1,0,3}};
        require(samples.load({group},{sample}));
        SC55Partial partial{};
        partial.used = true; partial.raw[1] = partial.raw[10] = partial.raw[13] = 64;
        std::array<uint8_t,12> scale; scale.fill(64);
        const auto plan = sc55::PreparePartialSample(partial,samples,60,60,60,scale,255,0x80,9);
        require(plan.has_value());
        for (bool unoffset : {false,true})
        {
            const auto setup = sc55::PreparePartialSamplePcm(*plan,samples,2,unoffset,10);
            require(setup && setup->address.mode == 0xa142 && setup->loopFlag == 2);
            require(setup->address.start == (unoffset ? 0x12ff00u : 0x130100u)
                && setup->address.loop == 0x130200 && setup->address.end == 0x130300);
            sc55::PreparedVoiceBatch::Entry entry;
            entry.channel = 2; entry.pcm.sample = setup->address;
            sc55::PreparedVoiceBatch batch;
            sc55::VoiceKeyMask mask{};
            std::array<sc55::VoiceStopState,24> voices{};
            pcm = std::make_unique<pcm_t>(); PCM_Init(*pcm,*mcu);
            require(batch.begin(std::span(&entry,1),mask));
            require(batch.advance(voices,mask,read,write) == sc55::PreparedVoiceBatch::Status::complete);
            require(word(0x1e) == 0xa142 && pcm->voice_mask == 4);
            // PCM address storage is 20-bit, unlike the retained 24-bit bus values.
            require(pcm->ram1[2][4] == (setup->address.start & 0xfffff)
                && pcm->ram1[2][2] == 0x30200 && pcm->ram1[2][0] == 0x30300);
        }
        require(!sc55::PreparePartialSamplePcm(*plan,samples,24,false,0));
        auto special = *plan; special.sampleId = 0x8000;
        require(!sc55::PreparePartialSamplePcm(special,samples,0,false,0));
    }
    {
        using Batch = sc55::PreparedVoiceBatch;
        using Status = Batch::Status;
        for (unsigned count : {1u,2u})
            for (bool post : {false,true})
            {
                pcm = std::make_unique<pcm_t>();
                PCM_Init(*pcm,*mcu);
                Batch batch;
                sc55::VoiceKeyMask mask{};
                std::array<sc55::VoiceStopState,24> voices{};
                std::array<Batch::Entry,2> entries{};
                entries[0].channel = 2; entries[1].channel = 19;
                for (const auto& entry : entries)
                {
                    voices[entry.channel].flagMinus3B = post ? 128 : 0;
                    pcm->ram2[entry.channel][9] = pcm->ram2[entry.channel][10] = 100;
                }
                require(batch.begin(std::span(entries).first(count),mask));
                require(!batch.begin(std::span(entries).first(count),mask));
                require(batch.advance(voices,mask,read,write) == Status::waitingForReuse);
                require(mask.enabled == 0);
                pcm->ram2[2][9] = 0;
                if (count == 2)
                {
                    require(batch.advance(voices,mask,read,write) == Status::waitingForReuse);
                    // The cursor has passed voice 2: it must not restart its
                    // readiness check after voice 19 becomes available.
                    pcm->ram2[2][9] = 100;
                    pcm->ram2[19][9] = 0;
                }
                require(batch.advance(voices,mask,read,write) == (post ? Status::waitingForKeyLatch : Status::complete));
                const uint32_t expectedMask = (1u<<2) | (count == 2 ? (1u<<19) : 0);
                require(mask.enabled == expectedMask && mask.prepared == 0 && pcm->voice_mask == expectedMask);
                if (post)
                {
                    pcm->ram2[2][7] |= 32;
                    if (count == 2)
                    {
                        require(batch.advance(voices,mask,read,write) == Status::waitingForKeyLatch);
                        // First voice's completed post-write must not repeat.
                        pcm->ram2[2][5] = 0x4321;
                        pcm->ram2[19][7] |= 32;
                    }
                    require(batch.advance(voices,mask,read,write) == Status::complete);
                    if (count == 2) require(pcm->ram2[2][5] == 0x4321);
                }
                require(batch.advance(voices,mask,[](uint8_t)->uint8_t { throw std::runtime_error("Completed batch read"); },
                    [](uint8_t,uint8_t) { throw std::runtime_error("Completed batch write"); }) == Status::complete);
            }
        Batch batch;
        sc55::VoiceKeyMask mask{};
        std::array<Batch::Entry,2> entries{};
        require(!batch.begin(entries,mask)); // duplicate slots
        entries[1].channel = 24;
        require(!batch.begin(entries,mask) && mask.prepared == 0);
        entries[1].channel = 1;
        require(batch.begin(entries,mask));
        std::array<sc55::VoiceStopState,24> voices{};
        voices[0].pendingOperation = sc55::VoiceOperation::finishStop;
        require(batch.advance(voices,mask,read,write) == Status::cancelled);
        require(mask.prepared == 3 && mask.enabled == 0);
        // A first voice can be cancelled while the second waits. The final
        // first-voice check must prevent the entire PCM commit.
        pcm = std::make_unique<pcm_t>();
        PCM_Init(*pcm,*mcu);
        mask = {}; voices = {};
        require(batch.begin(entries,mask));
        pcm->ram2[1][9] = pcm->ram2[1][10] = 10;
        require(batch.advance(voices,mask,read,write) == Status::waitingForReuse);
        voices[0].pendingOperation = sc55::VoiceOperation::finishStop;
        pcm->ram2[1][9] = 0;
        unsigned writes = 0;
        require(batch.advance(voices,mask,read,[&](uint8_t a,uint8_t v) {
            require(a == 0x3e && v == 1); ++writes; write(a,v);
        }) == Status::cancelled);
        require(writes == 1 && mask.enabled == 0 && mask.prepared == 3 && pcm->voice_mask == 0);
    }
    for (unsigned channel = 0; channel < 24; ++channel)
    {
        sc55::VoicePostEnable state{1,65535,0x2345};
        unsigned writes = 0, reads = 0;
        const auto postRead = [&](uint8_t a) { ++reads; return read(a); };
        const auto postWrite = [&](uint8_t a,uint8_t v) { ++writes; write(a,v); };
        require(sc55::PollVoicePostEnable(channel,state,postRead,postWrite) == true);
        require(writes == 0 && reads == 0);
        state.field65 = 0;
        pcm->ram2[channel][7] = 0;
        pcm->ram2[channel][5] = 0x5678;
        require(sc55::PollVoicePostEnable(channel,state,postRead,postWrite) == false);
        require(writes == 1 && reads == 3 && pcm->ram2[channel][5] == 0x5678);
        pcm->ram2[channel][7] = 32;
        write(0x3e,uint8_t((channel+1)%24)); // retry must reselect the right voice
        writes = reads = 0;
        const std::array<uint8_t,7> addresses{0x3e,0x1a,0x1b,0x36,0x37,0x1a,0x1b};
        const std::array<uint8_t,7> values{uint8_t(channel),0xff,0,0x7f,0xff,0x23,0x45};
        require(sc55::PollVoicePostEnable(channel,state,postRead,[&](uint8_t a,uint8_t v) {
            require(writes < addresses.size() && a == addresses[writes] && v == values[writes]);
            if (writes != 0) require(reads == 3);
            ++writes; write(a,v);
        }) == true);
        require(writes == 7 && reads == 3 && word(0x36) == 32767 && word(0x1a) == 0x2345);
    }
    require(!sc55::PollVoicePostEnable(24,{},[](uint8_t)->uint8_t {
        throw std::runtime_error("Invalid post-enable read");
    },[](uint8_t,uint8_t) { throw std::runtime_error("Invalid post-enable write"); }));
    {
        sc55::VoiceKeyMask mask{0xffffffff,0x00123456};
        sc55::VoiceStopState state;
        state.pendingOperation = sc55::VoiceOperation::finishStop;
        unsigned writes = 0, reads = 0;
        const auto maskWrite = [&](uint8_t a,uint8_t v) {
            require(a == writes && reads == 0);
            ++writes; write(a,v);
        };
        const auto maskRead = [&](uint8_t a) {
            require(a == 0 && writes == 4 && reads == 0);
            ++reads; return read(a);
        };
        require(!sc55::RemovePreparedVoiceKeys(mask,state,maskRead,maskWrite));
        require(writes == 0 && reads == 0 && mask.enabled == 0xffffffff);
        state.pendingOperation = sc55::VoiceOperation::none;
        require(sc55::RemovePreparedVoiceKeys(mask,state,maskRead,maskWrite));
        require(writes == 4 && reads == 1 && mask.enabled == 0xffedcba9 && mask.prepared == 0x00123456);
        require(pcm->voice_mask == 0x0fedcba9 && pcm->voice_mask_pending == 0x0fedcba9 && !pcm->voice_mask_updating);
        writes = reads = 0;
        sc55::EnablePreparedVoiceKeys(mask,maskRead,maskWrite);
        require(writes == 4 && reads == 1 && mask.enabled == 0xffffffff && mask.prepared == 0);
        require(pcm->voice_mask == 0x0fffffff && !pcm->voice_mask_updating);
    }
    {
        sc55::VoiceStopState state;
        state.fieldC8B3 = 7; state.fieldCB30 = 9;
        const sc55::PreparedVoicePcm setup{{0x1234,0x56bcde,0x78f012,0x9a3456},0x789a,0xbcde,0xf012,0x3456};
        require(!sc55::CommitPreparedVoice(24,state,setup,[](uint8_t,uint8_t) {
            throw std::runtime_error("Invalid commit wrote PCM");
        }));
        require(state.fieldC8B3 == 7 && state.fieldCB30 == 9);
        state.cached16 = 0x789a; state.cached18 = 0xbcde; state.pcm10 = 0xf012;
        const std::array<uint8_t,26> addresses{0x3e,0x1e,0x1f,5,6,7,9,10,11,13,14,15,
            0x12,0x13,0x14,0x15,0x1c,0x1d,0x1a,0x1b,0x16,0x17,0x18,0x19,0x10,0x11};
        const std::array<uint8_t,26> values{23,0x12,0x34,0x56,0xbc,0xde,0x78,0xf0,0x12,0x9a,0x34,0x56,
            0x78,0x9a,0xbc,0xde,0xf0,0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0,0x12};
        unsigned count = 0;
        require(sc55::CommitPreparedVoice(23,state,setup,[&](uint8_t address,uint8_t value) {
            require(count < addresses.size());
            require(address == addresses[count] && value == values[count]);
            ++count; write(address,value);
        }));
        require(count == addresses.size() && state.fieldC8B3 == 0 && state.fieldCB30 == 0);
        require(word(0x1e) == 0x1234 && word(0x1c) == 0xf012 && word(0x16) == 0x789a);
    }
    for (uint8_t flag : {uint8_t(0),uint8_t(128)})
        for (uint16_t delay : {uint16_t(0),uint16_t(1)})
        {
            sc55::VoiceStopState prepared;
            prepared.stages = {18,20,22}; prepared.savedStage = 6;
            prepared.progress = 1234; prepared.delayAccumulator = delay;
            prepared.cached18 = 0x64ba; prepared.pcm10 = 0x3456; prepared.flagMinus3B = flag;
            write(0x3e,0);
            sc55::ActivatePreparedVoice(prepared,write);
            if (flag == 0)
                require(prepared.stages == std::array<uint16_t,3>{6,20,22} && prepared.savedStage == 0 && prepared.progress == 1234);
            else
                require(prepared.stages == std::array<uint16_t,3>{uint16_t(delay ? 2 : 0),uint16_t(delay ? 2 : 0),uint16_t(delay ? 2 : 0)}
                    && prepared.savedStage == 6 && prepared.progress == (delay ? 1234 : 0));
            require(word(0x18) == (flag && !delay ? 0xb6 : 0x64ba));
            require(word(0x10) == (flag && !delay ? 0 : 0x3456));
        }
    for (unsigned channel = 0; channel < 24; ++channel)
    {
        pcm->ram2[channel][9] = 100; pcm->ram2[channel][10] = 200;
        require(sc55::PollVoiceReuse(uint8_t(channel),0,read,write) == sc55::VoiceReuseReadiness::pending);
        pcm->ram2[channel][9] = 0;
        require(sc55::PollVoiceReuse(uint8_t(channel),0,read,write) == sc55::VoiceReuseReadiness::ready);
        pcm->ram2[channel][9] = 100; pcm->ram2[channel][10] = 0;
        require(sc55::PollVoiceReuse(uint8_t(channel),0,read,write) == sc55::VoiceReuseReadiness::ready);
        require(sc55::PollVoiceReuse(uint8_t(channel),4,read,write) == sc55::VoiceReuseReadiness::cancelled);
    }
    require(!sc55::PollVoiceReuse(24,0,[](uint8_t)->uint8_t { throw std::runtime_error("Invalid reuse read"); },
        [](uint8_t,uint8_t) { throw std::runtime_error("Invalid reuse write"); }));
    for (unsigned mode : {0u,2u,1u})
        for (bool reserved : {false,true})
        {
            sc55::VoiceAllocator allocator;
            require(allocator.initializeTables());
            std::array<sc55::VoiceStopState,24> states{};
            for (unsigned i = 0; i < 12; ++i)
            {
                const auto created = allocator.createGroup({uint8_t(i%2),60,uint8_t(60+i),0,2});
                require(created.has_value());
                for (unsigned j = 0; j < 2; ++j) allocator.activity[created->voices[j]] = uint8_t(10+i);
            }
            sc55::VoiceCapacityPolicy policy;
            policy.modes.fill(uint8_t(mode));
            if (reserved) policy.reserves.fill(24);
            unsigned writes = 0;
            const auto result = sc55::EnsureVoiceCapacity(allocator,states,0,2,policy,read,
                [&](uint8_t a,uint8_t v) { ++writes; write(a,v); });
            require(result.has_value() && *result == (mode != 1));
            require(writes == (mode == 1 ? 0 : 6));
            if (*result) require(allocator.createGroup({0,60,80,0,2}).has_value());
        }
    // A corrupt later group must not stop an earlier valid matching group.
    // Selector zero bypasses traversal, even when the list itself is corrupt.
    for (unsigned fault = 0; fault < 3; ++fault)
    {
        sc55::VoiceAllocator allocator;
        require(allocator.initializeTables());
        const auto first = allocator.createGroup({0,60,42,0,2});
        const auto second = allocator.createGroup({0,60,46,0,2});
        require(first && second);
        std::array<sc55::VoiceStopState,24> states{};
        for (auto& state : states) state = {{{2,4,6}},0x1234,0x5678,7,static_cast<sc55::VoiceOperation>(9)};
        if (fault == 0) allocator.groups.tail[second->group] = 24;
        if (fault == 1) allocator.noteGroups[second->group].next = 24;
        if (fault == 2)
        {
            allocator.noteGroups[second->group].noteClass = 61;
            allocator.noteGroups[second->group].next = second->group;
        }
        const auto savedAllocator = allocator;
        const auto savedStates = states;
        unsigned io = 0;
        const auto load = [&](uint8_t) { ++io; return uint8_t(0); };
        const auto store = [&](uint8_t,uint8_t) { ++io; };
        require(!sc55::StopRhythmExclusiveGroups(allocator,states,0,60,load,store));
        require(io == 0 && std::memcmp(&allocator,&savedAllocator,sizeof allocator) == 0
            && std::memcmp(&states,&savedStates,sizeof states) == 0);
        const auto bypass = sc55::StopRhythmExclusiveGroups(allocator,states,0,0,load,store);
        require(bypass && *bypass == 0 && io == 0);
        require(!sc55::StopRhythmExclusiveGroups(allocator,states,16,60,load,store));
        require(io == 0 && std::memcmp(&allocator,&savedAllocator,sizeof allocator) == 0
            && std::memcmp(&states,&savedStates,sizeof states) == 0);
        allocator.noteGroups[second->group].key = 42;
        allocator.noteGroups[second->group].retirementFlags = 4;
        const auto beforeRepeated = allocator;
        std::array<uint8_t,16> retained; retained.fill(255);
        require(!sc55::RetireRepeatedNote(allocator,states,0,42,60,0x81,retained,load,store));
        require(io == 0 && std::memcmp(&allocator,&beforeRepeated,sizeof allocator) == 0
            && std::memcmp(&states,&savedStates,sizeof states) == 0);
        retained[0] = 42;
        const auto protectedKey = sc55::RetireRepeatedNote(allocator,states,0,42,60,0x81,retained,load,store);
        require(protectedKey && !*protectedKey && io == 0);
    }
    {
        sc55::RhythmKeyMap map;
        map.pitches[60] = 36;
        const auto keys = sc55::PrepareRhythmInitialKeys(60,map,76,255);
        require(keys && keys->initialKey == 47 && keys->sourceKey == 47);
        require(!sc55::PrepareRhythmInitialKeys(128,map,64,0));
        map.pitches[60] = 0;
        require(sc55::PrepareRhythmInitialKeys(60,map,63,0)->initialKey == 0);
        map.pitches[60] = 127;
        require(sc55::PrepareRhythmInitialKeys(60,map,65,0)->initialKey == 127);
    }
    for (unsigned scenario = 0; scenario < 4; ++scenario)
    {
        sc55::VoiceAllocator allocator; require(allocator.initializeTables());
        std::array<sc55::VoiceStopState,24> states{};
        for (unsigned i = 0; i < 12; ++i) require(bool(allocator.createGroup({0,60,42,0,2})));
        sc55::VoiceCapacityPolicy policy; policy.modes.fill(1); policy.reserves.fill(24);
        std::array<uint8_t,16> retained; retained.fill(255);
        if (scenario == 3) allocator.groups.tail[allocator.partHead[0]] = 24;
        sc55::RhythmGroupAdmission pending({0,uint8_t(scenario == 1 ? 0 : 60),99,2,uint8_t(scenario == 2 ? 0 : 2)},
            0,retained,policy);
        unsigned io = 0;
        const auto load = [&](uint8_t) { ++io; return uint8_t(0); };
        const auto store = [&](uint8_t,uint8_t) { ++io; };
        using Status = sc55::RhythmGroupAdmission::Status;
        const std::array expected{Status::allocated,Status::capacityRejected,Status::invalidInput,Status::failed};
        require(pending.run(allocator,states,load,store) == expected[scenario]);
        if (scenario == 0) require(pending.group() && allocator.freeCount == 22 && io > 0);
        else require(!pending.group() && io == 0 && allocator.freeCount == 0);
        const auto saved = allocator; const auto savedStates = states; const auto beforeIo = io;
        require(pending.run(allocator,states,load,store) == expected[scenario] && io == beforeIo);
        require(std::memcmp(&saved,&allocator,sizeof allocator) == 0 && std::memcmp(&savedStates,&states,sizeof states) == 0);
    }
    unsigned reclaimChecks = 0;
    for (unsigned part = 0; part < 16; ++part)
        for (bool prepend : {false,true})
            for (unsigned count : {1u,2u})
            {
                sc55::VoiceAllocator allocator;
                require(allocator.initializeTables());
                const auto allocated = allocator.createGroup({uint8_t(part),60,100,0,uint8_t(count)});
                require(allocated.has_value());
                std::array<sc55::VoiceStopState,24> stopState;
                for (auto& state : stopState) state = {{{2,4,6}},0x1234,0x5678,7,static_cast<sc55::VoiceOperation>(9)};
                for (auto& registers : pcm->ram2) { registers[3] = 0x1234; registers[4] = 0x5678; }
                for (unsigned i = 0; i < count; ++i)
                {
                    const auto voice = allocated->voices[i];
                    pcm->ram2[voice][9] = 100;
                    pcm->ram2[voice][10] = i == 0 ? 50 : 200;
                }
                allocator.shortage = uint8_t(count);
                unsigned writes = 0;
                const auto next = sc55::StopAndReclaimGroup(allocator,stopState,allocated->group,part,prepend,read,
                    [&](uint8_t a,uint8_t v) { ++writes; write(a,v); });
                require(next && *next == 255 && writes == count*3 && allocator.shortage == 0 && allocator.freeCount == 24);
                for (unsigned i = 0; i < count; ++i)
                {
                    const auto voice = allocated->voices[i];
                    const auto& state = stopState[voice];
                    require(state.stages == std::array<uint16_t,3>{uint16_t(i == 0 ? 0x12 : 0x14),uint16_t(i == 0 ? 0x12 : 0x14),uint16_t(i == 0 ? 0x12 : 0x14)});
                    require(state.fieldCB30 == 0 && state.pendingOperation == sc55::VoiceOperation::finishStop);
                    require(state.cached16 == (i == 0 ? 0xb6 : 0x1234) && state.cached18 == (i == 0 ? 0x5678 : 0xb6));
                    require(pcm->ram2[voice][9] == 100 && pcm->ram2[voice][10] == (i == 0 ? 50 : 200));
                }
                // No PCM clock advancement/silence wait before native reuse.
                const auto reused = allocator.createGroup({uint8_t(part),61,100,0,uint8_t(count)});
                require(reused.has_value());
                if (prepend) require(reused->voices == allocated->voices);
                writes = 0;
                allocator.groups.tail[reused->group] = 24;
                require(!sc55::StopAndReclaimGroup(allocator,stopState,reused->group,part,prepend,read,
                    [&](uint8_t a,uint8_t v) { ++writes; write(a,v); }) && writes == 0);
                ++reclaimChecks;
            }
    std::printf("Native stop/reclaim/reallocate: %u real-PCM cases passed\n",reclaimChecks);
    unsigned stopChecks = 0;
    unsigned restartChecks = 0;
    for (unsigned slot = 0; slot < 24; ++slot)
        for (bool restart : {false,true})
            for (bool absent : {false,true})
                for (bool first : {false,true})
                {
                    sc55::VoiceAllocator allocator; require(allocator.initializeTables());
                    // Reserve all slots so every logical slot has valid ownership.
                    for (unsigned i = 0; i < 24; ++i) require(bool(allocator.createGroup({1,0x80,60,1,1})));
                    sc55::VoiceInstallationState installed;
                    installed.pendingRelease[slot] = 77;
                    sc55::VoiceStopState state;
                    state.stages = {2,4,6}; state.cached16 = 0x1234; state.cached18 = 0x5678;
                    state.fieldCB30 = 99; state.pendingOperation = static_cast<sc55::VoiceOperation>(9);
                    pcm->ram2[slot][9] = 100; pcm->ram2[slot][10] = first ? 50 : 200;
                    uint8_t flags = restart ? 0xa0 : 0x20;
                    const sc55::VoiceInstallationInput input{0,uint16_t(absent ? 0xffff : 0),0,1,60,60,100,0,60,false};
                    unsigned io = 0;
                    require(sc55::RestartAndInstallVoice(slot,input,flags,allocator,installed,state,
                        [&](uint8_t a) { ++io; return read(a); },[&](uint8_t a,uint8_t v) { ++io; write(a,v); }));
                    require(io == (restart ? 9 : 0));
                    require(state.pendingOperation == (absent ? static_cast<sc55::VoiceOperation>(9) : sc55::VoiceOperation::prepare));
                    require(state.fieldCB30 == (restart ? 0 : 99));
                    require(state.cached16 == (restart && first ? 0xb6 : 0x1234));
                    require(state.cached18 == (restart && !first ? 0xb6 : 0x5678));
                    require(state.stages == (restart ? std::array<uint16_t,3>{uint16_t(first ? 18 : 20),uint16_t(first ? 18 : 20),uint16_t(first ? 18 : 20)} : std::array<uint16_t,3>{2,4,6}));
                    require(installed.pendingRelease[slot] == (absent ? 77 : 0));
                    require(allocator.freeCount == (absent ? 1 : 0));
                    if (!absent) require(installed.voices[slot].input.restarted == restart);
                    // Invalid return links cannot emit even the channel-select write.
                    allocator.allocations[slot].noteGroup = 24;
                    auto invalid = input; invalid.sample = 0xffff;
                    flags = 128; io = 0;
                    require(!sc55::RestartAndInstallVoice(slot,invalid,flags,allocator,installed,state,
                        [&](uint8_t a) { ++io; return read(a); },[&](uint8_t a,uint8_t v) { ++io; write(a,v); }));
                    require(io == 0);
                    ++restartChecks;
                }
    std::printf("Native restart/install: %u real-PCM cases passed\n",restartChecks);
    for (unsigned channel = 0; channel < 24; ++channel)
        for (unsigned value = 0; value < 65536; value += 257)
            for (uint16_t second : {uint16_t(value),uint16_t(value-1),uint16_t(value+1)})
            {
                for (auto& registers : pcm->ram2) { registers[3] = 0x1234; registers[4] = 0x5678; }
                pcm->ram2[channel][9] = uint16_t(value); pcm->ram2[channel][10] = second;
                std::array<uint8_t,9> operations{};
                unsigned count = 0;
                const auto stopped = sc55::StopVoicePcm(uint8_t(channel),[&](uint8_t a) {
                    require(count < operations.size()); operations[count++] = a; return read(a);
                },[&](uint8_t a,uint8_t v) {
                    require(count < operations.size()); operations[count++] = uint8_t(a|128); write(a,v);
                });
                const bool first = second < value;
                require(stopped && stopped->pcmAddress == (first ? 0x16 : 0x18));
                require(count == 9 && operations == std::array<uint8_t,9>{0xbe,0x32,0x3a,0x3b,0x34,0x3a,0x3b,
                    uint8_t(first ? 0x96 : 0x98),uint8_t(first ? 0x97 : 0x99)});
                require(word(0x16) == (first ? 0xb6 : 0x1234) && word(0x18) == (first ? 0x5678 : 0xb6));
                require(word(0x32) == value && word(0x34) == second); // command is not immediate silence
                for (unsigned other = 0; other < 32; ++other)
                    if (other != channel) require(pcm->ram2[other][3] == 0x1234 && pcm->ram2[other][4] == 0x5678);
                ++stopChecks;
            }
    require(!sc55::StopVoicePcm(24,[](uint8_t)->uint8_t { throw std::runtime_error("Unexpected stop read"); },
        [](uint8_t,uint8_t) { throw std::runtime_error("Unexpected stop write"); }));
    std::printf("Native PCM voice stop: %u register cases passed\n",stopChecks);
    for (unsigned channel = 0; channel < 32; ++channel)
        for (unsigned value = 0; value < 65536; value += 257)
            for (bool moving : {false,true})
            {
                // Sentinel values catch writes to the wrong physical channel.
                for (auto& registers : pcm->ram2) { registers[4] = 0x3456; registers[10] = 0xabcd; }
                write(0x3e,uint8_t(channel));
                auto state = initial;
                state.level = uint16_t(value); state.pcmWord = moving ? 0x64ba : 0xff00;
                Runner runner(setup,state);
                pcm->ram2[channel][10] = uint16_t(value);
                sc55::SynchronizeEnvelopePcm(runner,read,write);
                require(runner.state().level == (moving ? uint16_t(value*2) : uint16_t(value)));
                require(word(0x18) == (moving ? 0xff00 : 0x3456));
                require(word(0x34) == (moving ? uint16_t(value) : uint16_t(value >> 1)));
                for (unsigned other = 0; other < 32; ++other)
                    if (other != channel) require(pcm->ram2[other][4] == 0x3456 && pcm->ram2[other][10] == 0xabcd);
                require(runner.state().segment.progress.position == 0 && runner.state().segment.stage == Stage::attack1);
                ++syncChecks;
            }
    // Actual native state drives real PCM command registers through a note
    // lifetime. No audio frames are generated in this register-level test.
    sc55::EnvelopeTimes times{};
    Runner runner(setup,initial); runner.activateNewVoice();
    write(0x3e,7);
    for (unsigned stage = 1; stage <= 4; ++stage)
    {
        require(runner.tick(1,{},times)); sc55::WriteEnvelopePcm(runner,write);
        require(word(0x18) == uint16_t((setup.targets[stage-1]<<8)|0xaf));
    }
    require(runner.tick(1,{},times)); sc55::WriteEnvelopePcm(runner,write);
    require(word(0x18) == 0xff00);
    sc55::SynchronizeEnvelopePcm(runner,read,write);
    require(word(0x34) == 5120);
    runner.release(runner.state().level);
    require(runner.tick(1,{},times)); sc55::WriteEnvelopePcm(runner,write);
    require(word(0x18) == 0x00af);
    require(runner.tick(1,{},times) && runner.state().segment.stage == Stage::finished);
    require(mcu->pc == 0 && mcu->cycles == 0);
    // Advance the real slot pipeline, not just register storage. This clock
    // is a synthetic integration fixture, NOT the recovered firmware clock.
    pcm = std::make_unique<pcm_t>();
    PCM_Init(*pcm,*mcu); PCM_UseSimulation(*pcm,false);
    uint64_t outputFrames = 0;
    mcu->callback_userdata = &outputFrames;
    mcu->sample_callback = [](void* context,const AudioFrame<int32_t>&) { ++*static_cast<uint64_t*>(context); };
    write(0x3d,23); // 24 physical slots, 625 device cycles per pipeline pass.
    write(0x3e,0);
    sc55::VoiceKeyMask keyMask{};
    sc55::PreparedVoiceBatch batch;
    std::array<sc55::PreparedVoiceBatch::Entry,2> batchEntries{};
    batchEntries[1].channel = 1;
    std::array<sc55::VoiceStopState,24> batchVoices{};
    batchVoices[0].flagMinus3B = batchVoices[1].flagMinus3B = 128;
    runner = Runner(setup,initial);
    sc55::PrepareVoiceAmplitude(batchVoices[0],runner);
    sc55::PrepareVoiceAmplitude(batchVoices[1],runner);
    require(batch.begin(batchEntries,keyMask));
    require(batch.advance(batchVoices,keyMask,read,write) == sc55::PreparedVoiceBatch::Status::waitingForKeyLatch);
    PCM_Update(*pcm,2500); // Allow key-on to reach the slot pipeline.
    require(batch.advance(batchVoices,keyMask,read,write) == sc55::PreparedVoiceBatch::Status::complete);
    require(keyMask.enabled == 3 && keyMask.prepared == 0 && pcm->voice_mask == 3);
    write(0x3e,0); // The envelope below owns the first voice.
    times.fill(256);
    const auto continued = sc55::ContinueVoiceAmplitude(batchVoices[0],runner);
    require(continued.has_value());
    runner = *continued;
    bool rose = false, fellAfterRelease = false;
    uint16_t peak = 0;
    unsigned ticks = 0;
    for (; ticks < 256; ++ticks)
    {
        sc55::SynchronizeEnvelopePcm(runner,read,write);
        if (ticks == 160) runner.release(runner.state().level);
        require(runner.tick(1,{},times));
        if (runner.state().segment.stage == Stage::finished) break;
        sc55::WriteEnvelopePcm(runner,write);
        PCM_Update(*pcm,pcm->cycles+128*625);
        const auto actual = word(0x34);
        rose |= actual > 0;
        if (actual > peak) peak = actual;
        if (ticks > 160 && actual < peak) fellAfterRelease = true;
    }
    require(rose && fellAfterRelease && ticks > 160 && ticks < 256 && outputFrames > 0);
    // Logical completion can precede the chip's interpolated level reaching
    // zero. Voice-owner cleanup is a separate operation, not a runner reset.
    require(runner.state().segment.stage == Stage::finished);
    require(runner.state().level == sc55::DecodeEnvelopePcmLevel(word(0x34)));
    const auto residual = word(0x34);
    require(residual != 0); // Exercise actual residual removal, not silence.
    require(sc55::WriteEnvelopeTermination(0,write));
    require(word(0x18) == 0x00b6 && word(0x34) == residual);
    PCM_Update(*pcm,pcm->cycles+128*625);
    require(word(0x34) == 0);
    for (unsigned channel = 0; channel < 32; ++channel)
    {
        for (auto& registers : pcm->ram2) registers[4] = 0x3456;
        require(sc55::WriteEnvelopeTermination(uint8_t(channel),write));
        for (unsigned other = 0; other < 32; ++other)
            require(pcm->ram2[other][4] == (other == channel ? 0x00b6 : 0x3456));
    }
    unsigned invalidWrites = 0;
    require(!sc55::WriteEnvelopeTermination(32,[&](uint8_t,uint8_t) { ++invalidWrites; }) && invalidWrites == 0);
    require(mcu->pc == 0 && mcu->cycles == 0);
    std::printf("Native timed envelope / PCM: %u control ticks, %llu output callbacks, rise/release verified\n",
        ticks,static_cast<unsigned long long>(outputFrames));
    std::printf("Native envelope / real PCM: %u synchronization cases and command lifetime passed without control ROM\n",syncChecks);
    return 0;
}
