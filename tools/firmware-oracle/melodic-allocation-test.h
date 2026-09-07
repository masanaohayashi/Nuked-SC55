#pragma once
#include "sc55_note_dispatch.h"
#include <cstring>
#include <cstdio>
#include <type_traits>
#include <stdexcept>

inline void verifyMelodicAllocation(const sc55::SoundData& data)
{
    const auto require = [](bool ok) { if (!ok) throw std::runtime_error("Melodic allocation regression"); };
    // All allocator fields are byte tables. Reject this comparison at compile
    // time if a future field introduces padding or a nonunique representation.
    static_assert(std::has_unique_object_representations_v<sc55::VoiceAllocator>);
    const auto same = [](const auto& a,const auto& b) { return std::memcmp(&a,&b,sizeof(a)) == 0; };
    using Status = sc55::MelodicAllocationResult::Status;
    unsigned cases = 0, singles = 0, pairs = 0, shortages = 0;
    for (unsigned program = 0; program < 128; ++program)
        for (unsigned velocity = 1; velocity < 128; ++velocity)
            for (unsigned soft = 0; soft < 2; ++soft)
                for (unsigned capacity : {0u,1u,24u})
                {
                    sc55::ChannelControls channels;
                    require(channels.apply({sc55::MidiDecoder::Kind::message,0xc0,uint8_t(program),0,1}));
                    require(channels.apply({sc55::MidiDecoder::Kind::message,0xb0,67,uint8_t(soft*127),2}));
                    const sc55::MidiDecoder::Event event{sc55::MidiDecoder::Kind::message,0x90,60,uint8_t(velocity),2};
                    const auto selected = sc55::PrepareMelodicNoteVelocity(event,channels.channel(0),0,false,0,data);
                    require(bool(selected));
                    sc55::VoiceAllocator actual; require(actual.initializeTables(capacity == 0 ? 1 : capacity));
                    if (capacity == 0) require(bool(actual.takeFreeVoice()));
                    actual.activity.fill(17); actual.fieldA3E0.fill(23);
                    const auto before = actual; auto expected = actual;
                    const uint8_t part = uint8_t(program%16);
                    const auto result = sc55::AllocateMelodicNote(event,channels.channel(0),
                        {0,false,0,part,0x80,1},data,actual);
                    require(result.selection && result.selection->tone == selected->tone
                        && result.selection->partials.candidates.flags == selected->partials.candidates.flags);
                    const auto count = selected->partials.candidates.count;
                    if (count == 0)
                        require(result.status == Status::velocityRejected && !result.group && same(actual,before));
                    else if (capacity < count)
                    { require(result.status == Status::needsCapacity && !result.group && same(actual,before)); ++shortages; }
                    else
                    {
                        const auto group = expected.createGroup({part,0x80,selected->note,1,uint8_t(count)});
                        require(group && result.group && result.status == Status::allocated);
                        const auto dispatch = sc55::PlanPartialVoiceDispatch(*data.patch(selected->tone),
                            selected->partials.candidates.flags,{group->voices[0],group->voices[1]});
                        require(dispatch && result.group->group == group->group && result.group->voices == group->voices);
                        for (unsigned i = 0; i < 2; ++i)
                            require(result.dispatch[i].prepare == (*dispatch)[i].prepare
                                && result.dispatch[i].voice == (*dispatch)[i].voice);
                        require(same(actual,expected));
                        if (count == 1) ++singles; else ++pairs;
                    }
                    ++cases;
                }
    require(cases == 97536 && singles && pairs && shortages);
    sc55::ChannelControls channels;
    sc55::VoiceAllocator allocator; require(allocator.initializeTables());
    const sc55::MidiDecoder::Event note{sc55::MidiDecoder::Kind::message,0x90,60,100,2};
    for (unsigned part = 0; part < 16; ++part)
        for (unsigned bypass : {0u,255u})
            for (sc55::NoteKeyRange range : {sc55::NoteKeyRange{60,60},{61,127},{0,59},{127,0}})
            {
                auto actual = allocator;
                const bool accept = (part < 8 && bypass != 0) || (range.low == 60 && range.high == 60);
                const auto result = sc55::AllocateMelodicNote(note,channels.channel(0),
                    {0,false,0,uint8_t(part),0x80,1,range,uint8_t(bypass)},data,actual);
                if (accept) require(result.status == Status::allocated && result.group);
                else require(result.status == Status::keyRangeRejected && !result.selection
                    && !result.group && same(actual,allocator));
            }
    require(!sc55::AcceptNoteKeyRange(16,60,{},255));
    require(!sc55::AcceptNoteKeyRange(0,128,{},255));
    // Neutral identity, depth0's substitute, fractional scaling, low/high
    // saturation and adjusted-zero rejection. Compare the entire allocation
    // against explicit effective-velocity events, without a second adjustment.
    for (const auto adjustment : {sc55::PartVelocityAdjustment{64,64},{0,64},{1,64},
        {63,65},{127,127},{127,0},{64,63},{64,65}})
        for (unsigned velocity = 1; velocity < 128; ++velocity)
        {
            auto event = note; event.second = uint8_t(velocity);
            const auto effective = sc55::AdjustPartNoteVelocity(velocity,adjustment);
            require(bool(effective));
            auto actual = allocator, expected = allocator;
            sc55::MelodicAllocationInputs input{0,false,0,0,0x80,1};
            input.velocityAdjustment = adjustment;
            const auto result = sc55::AllocateMelodicNote(event,channels.channel(0),input,data,actual);
            if (*effective == 0)
                require(result.status == Status::velocityRejected && !result.selection && !result.group
                    && same(actual,allocator));
            else
            {
                event.second = *effective; input.velocityAdjustment = {};
                const auto direct = sc55::AllocateMelodicNote(event,channels.channel(0),input,data,expected);
                require(result.status == direct.status && result.selection && direct.selection
                    && result.selection->velocity == *effective && result.selection->note == note.first
                    && result.selection->partials.candidates.flags == direct.selection->partials.candidates.flags
                    && same(actual,expected));
            }
        }
    require(!sc55::AdjustPartNoteVelocity(0,{}));
    require(!sc55::AdjustPartNoteVelocity(128,{}));
    require(!sc55::AdjustPartNoteVelocity(100,{128,64}));
    require(!sc55::AdjustPartNoteVelocity(100,{64,128}));
    for (unsigned malformed = 0; malformed < 4; ++malformed)
    {
        auto actual = allocator;
        sc55::MelodicAllocationInputs input{0,false,0,0,0x80,1};
        auto event = note;
        if (malformed == 0) event.second = 0;
        if (malformed == 1) input.part = 16;
        if (malformed == 2) input.rhythm = true;
        if (malformed == 3) actual.freeHead = 24;
        const auto before = actual;
        require(sc55::AllocateMelodicNote(event,channels.channel(0),input,data,actual).status == Status::invalidInput);
        require(same(actual,before));
    }
    std::printf("Native melodic allocation: %u cases, %u single/%u paired allocations, %u capacity deferrals\n",
        cases,singles,pairs,shortages);
}
