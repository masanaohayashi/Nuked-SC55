#include "sc55_midi.h"
#include "sc55_midi_queue.h"
#include "sc55_channel.h"
#include "sc55_preset.h"
#include "sc55_note_fanout.h"
#include "sc55_rhythm.h"
#include <array>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <cstdio>

using Decoder = sc55::MidiDecoder;
using Kind = Decoder::Kind;
static void require(bool value) { if (!value) throw std::runtime_error("MIDI decoder regression"); }

static void verifyQueue(std::span<const uint8_t> bytes)
{
    using Queue = sc55::MidiEventQueue<2>;
    using Result = sc55::MidiDispatchResult;
    Decoder direct;
    std::vector<Decoder::Event> expected,actual;
    direct.push(bytes,[&](auto event) { expected.push_back(event); });
    Queue queue;
    std::size_t cursor = 0, calls = 0;
    while (cursor < bytes.size() || queue.size())
    {
        require(++calls <= bytes.size()*3+1);
        const auto chunk = bytes.subspan(cursor,std::min(bytes.size()-cursor,1+calls%13));
        const auto pushed = queue.push(chunk,55);
        require(pushed.status == Queue::PushStatus::accepted || pushed.status == Queue::PushStatus::full);
        require(pushed.consumed <= chunk.size()); cursor += pushed.consumed;
        unsigned earlyCalls = 0;
        const auto early = queue.dispatchOne(54,[&](auto) { ++earlyCalls; return Result::accepted; });
        require(earlyCalls == 0 && (early == Result::future || early == Result::idle));
        queue.dispatchOne(55,[&](auto event) { actual.push_back(event); return Result::accepted; });
    }
    require(actual.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        require(actual[i].kind == expected[i].kind && actual[i].status == expected[i].status
            && actual[i].first == expected[i].first && actual[i].second == expected[i].second
            && actual[i].dataSize == expected[i].dataSize);
}

int main()
{
    {
        std::array<uint8_t,128> accumulators;
        sc55::RhythmKeyMap map;
        for (unsigned key = 0; key < 128; ++key)
        {
            accumulators[key] = uint8_t(key*37);
            map.groups[key] = uint8_t(key+1); map.flags[key] = uint8_t(key^0x5a);
        }
        for (unsigned key = 0; key < 128; ++key)
            for (unsigned program = 0; program < 128; ++program)
                for (uint16_t tone : {uint16_t(0),uint16_t(223),uint16_t(224),uint16_t(32767),uint16_t(32768),uint16_t(65535)})
                {
                    auto selectedMap = map; selectedMap.tones[key] = tone;
                    const auto result = sc55::SelectRhythmTone(key,program,selectedMap,accumulators,73);
                    require(result && result->key == key && result->tone == tone
                        && result->flags == map.flags[key] && result->group == map.groups[key]
                        && result->velocityAccumulator == (program == 127 ? accumulators[key] : 73)
                        && result->present() == (tone < 32768));
                    if (result->present()) require(result->bank() == (tone < 224 ? 1 : 2)
                        && result->bankTone() == (tone < 224 ? tone : tone-224));
                    require(!sc55::SelectRhythmTone((key+1)%128,program,selectedMap,accumulators,73)->present());
                }
        require(!sc55::SelectRhythmTone(128,0,map,accumulators,0));
        require(!sc55::SelectRhythmTone(0,128,map,accumulators,0));
        std::puts("Rhythm tone mapping: key isolation, absent entries, bank boundary and program127 accumulator passed");
    }
    {
        using Status = sc55::PartNoteOnSelection::Status;
        for (unsigned flags = 0; flags < 256; ++flags)
            for (unsigned enabled = 0; enabled < 4; ++enabled)
            {
                sc55::PartNoteOnInputs input;
                input.noteFlags = uint8_t(flags);
                input.rhythmKeyFlags = {uint8_t((enabled&1) ? 0xff : 0xef),
                    uint8_t((enabled&2) ? 0xff : 0xef)};
                const bool accepts = !(flags&0x10) || (enabled&((flags&0x20) ? 1 : 2));
                const auto selected = sc55::SelectPartNoteOn(9,60,100,input);
                require(selected.status == (accepts ? Status::prepare : Status::rhythmRejected));
                if (accepts) require(selected.velocity == 100);
                input.velocityAdjustment = {1,64}; input.keyRange = {127,0};
                require(sc55::SelectPartNoteOn(9,60,1,input).status
                    == (accepts ? Status::velocityRejected : Status::rhythmRejected));
                input.velocityAdjustment = {128,128};
                // Zero Note On releases even disabled drum keys, regardless
                // of invalid/irrelevant adjustment or inverted key limits.
                require(sc55::SelectPartNoteOn(9,60,0,input).status == Status::release);
                require(sc55::SelectPartNoteOn(9,60,100,input).status
                    == (accepts ? Status::invalidInput : Status::rhythmRejected));
            }
        require(sc55::SelectPartNoteOn(16,60,0,{}).status == Status::invalidInput);
        require(sc55::SelectPartNoteOn(0,128,0,{}).status == Status::invalidInput);
        require(sc55::SelectPartNoteOn(0,60,128,{}).status == Status::invalidInput);
        std::puts("Part Note On: both rhythm maps, bit isolation, zero-release and rejection ordering passed");
    }
    {
        using Fanout = sc55::NoteOnFanout;
        const Decoder::Event note{Kind::message,0x93,60,100,2};
        for (unsigned mask = 0; mask < 65536; ++mask)
        {
            std::array<sc55::PartMidiReceive,16> routing;
            for (unsigned part = 0; part < 16; ++part)
                routing[part] = {3,uint16_t(mask&(1u<<part) ? 0x0200 : 0),0x80};
            Fanout fanout; require(fanout.begin(note,routing,{}));
            require(fanout.remainingParts() == mask);
            for (unsigned part = 16; part-- > 0;)
                if (mask&(1u<<part))
                {
                    const auto remaining = fanout.remainingParts();
                    require(!fanout.begin(note,routing,{}));
                    require(fanout.advance([&](uint8_t p,const auto& e) {
                        require(p == part && e.first == 60 && e.second == 100); return Fanout::Visit::deferred;
                    }) == Fanout::Status::deferred);
                    require(fanout.remainingParts() == remaining);
                    fanout.advance([&](uint8_t p,const auto&) { require(p == part); return Fanout::Visit::accepted; });
                    require(fanout.remainingParts() == (remaining&~(1u<<part)));
                }
            require(fanout.status() == Fanout::Status::complete);
            unsigned replay = 0;
            require(fanout.advance([&](auto,const auto&) { ++replay; return Fanout::Visit::accepted; }) == Fanout::Status::complete);
            require(replay == 0);
        }
        Fanout failed;
        std::array<sc55::PartMidiReceive,16> routing; routing[9] = {3,0x0200,0x80};
        require(failed.begin(note,routing,{}));
        require(failed.advance([](auto,const auto&) { return Fanout::Visit::failed; }) == Fanout::Status::failed);
        require(!failed.begin(note,routing,{}));
        unsigned replay = 0;
        require(failed.advance([&](auto,const auto&) { ++replay; return Fanout::Visit::accepted; }) == Fanout::Status::failed);
        require(replay == 0);
        Fanout invalid; auto zero = note; zero.second = 0;
        require(!invalid.begin(zero,routing,{}));
        std::puts("Note On fan-out: all65536 receive masks, descending order, deferral and non-replay passed");
    }
    {
        sc55::ChannelControls controls;
        sc55::LevelInputs level; sc55::SpatialInputs spatial;
        sc55::ApplyChannelOutputControls(controls.channel(0),level,spatial);
        // H8 voice points to part80b8: +0e=0 (CC93), +0f=40 (CC91).
        require(spatial.reverb == 0 && spatial.chorus == 40);
    }
    // Deliberately feed each byte separately: host packet boundaries carry no meaning.
    Decoder decoder;
    std::vector<Decoder::Event> events;
    auto sink = [&](auto e) { events.push_back(e); };
    const uint8_t stream[] {0x90,60,0xf8,100,61,0,0xc2,7,8,0xd2,70,0xf2,1,2,60,90};
    for (const auto byte : stream) decoder.push(std::span(&byte,1),sink);
    require(events.size() == 7);
    require(events[0].kind == Kind::realtime && events[0].status == 0xf8);
    require(events[1].status == 0x90 && events[1].first == 60 && events[1].second == 100);
    require(events[2].status == 0x90 && events[2].first == 61 && events[2].second == 0);
    require(events[3].status == 0xc2 && events[3].first == 7 && events[3].dataSize == 1);
    require(events[4].status == 0xc2 && events[4].first == 8);
    require(events[5].status == 0xd2 && events[5].first == 70);
    require(events[6].status == 0xf2 && events[6].first == 1 && events[6].second == 2);

    events.clear();
    const uint8_t sx[] {0xf0,0x41,0xfe,0x10,0xf7,12,13,0x91,62,1,0xf0,0x41,0x81,62,0};
    decoder.push(sx,sink);
    require(events.size() == 10);
    require(events[0].kind == Kind::sysexBegin);
    require(events[1].kind == Kind::sysexData && events[1].first == 0x41);
    require(events[2].kind == Kind::realtime);
    require(events[4].kind == Kind::sysexEnd);
    require(events[5].status == 0x91);
    require(events[8].kind == Kind::sysexAbort);
    require(events[9].status == 0x81 && events[9].first == 62);

    // Streaming SysEx must not truncate a bulk dump, even with real-time bytes.
    events.clear();
    std::vector<uint8_t> bulk(65536,0x01);
    bulk.front() = 0xf0; bulk.back() = 0xf7; bulk[123] = 0xf8;
    decoder.push(bulk,sink);
    require(events.size() == bulk.size());
    require(events.back().kind == Kind::sysexEnd);
    verifyQueue(stream); verifyQueue(sx); verifyQueue(bulk);
    {
        using Queue = sc55::MidiEventQueue<2>;
        using Result = sc55::MidiDispatchResult;
        Queue queue;
        const std::array<uint8_t,3> aborted{0xf0,0xf8,0xf0};
        auto pushed = queue.push(aborted,10);
        require(pushed.status == Queue::PushStatus::full && pushed.consumed == 2 && queue.size() == 2);
        require(queue.dispatchOne(10,[](auto) { return Result::deferred; }) == Result::deferred);
        require(queue.size() == 2);
        require(queue.dispatchOne(10,[](auto e) { require(e.kind == Kind::sysexBegin); return Result::accepted; }) == Result::accepted);
        pushed = queue.push(std::span(aborted).subspan(2),10);
        require(pushed.status == Queue::PushStatus::full && pushed.consumed == 0 && queue.size() == 1);
        require(queue.dispatchOne(10,[](auto e) { require(e.kind == Kind::realtime); return Result::accepted; }) == Result::accepted);
        pushed = queue.push(std::span(aborted).subspan(2),10);
        require(pushed.status == Queue::PushStatus::accepted && pushed.consumed == 1 && queue.size() == 2);
        require(queue.push({},9).status == Queue::PushStatus::invalidTime && queue.size() == 2);
        require(queue.dispatchOne(10,[](auto e) { require(e.kind == Kind::sysexAbort); return Result::accepted; }) == Result::accepted);
        require(queue.dispatchOne(10,[](auto e) { require(e.kind == Kind::sysexBegin); return Result::failed; }) == Result::failed);
        require(queue.failed() && queue.size() == 1);
        unsigned replayed = 0;
        require(queue.dispatchOne(11,[&](auto) { ++replayed; return Result::accepted; }) == Result::failed);
        require(queue.push(aborted,11).status == Queue::PushStatus::failed && replayed == 0);
    }
    decoder.reset();
    events.clear();
    const uint8_t interrupted[] {0x90,60,0xc0,5,0xf6,60,1};
    decoder.push(interrupted,sink);
    require(events.size() == 2 && events[0].status == 0xc0 && events[1].status == 0xf6);
    sc55::ChannelControls controls;
    const auto control = [&](uint8_t number, uint8_t value) {
        return controls.apply({Kind::message,0xb3,number,value,2});
    };
    require(!control(6,127));
    for (unsigned value = 0; value < 128; ++value)
    {
        require(control(67,uint8_t(value)));
        require(controls.channel(3).softPedal == (value >= 64));
        require(!controls.channel(2).softPedal);
    }
    require(control(101,0) && control(100,2));
    for (unsigned value = 0; value < 128; ++value)
    {
        require(control(6,uint8_t(value)));
        const int expected = int(value < 40 ? 40 : value > 88 ? 88 : value)-64;
        require(controls.channel(3).coarseTuning == expected);
        require(controls.channel(2).coarseTuning == 0);
    }
    require(!control(99,0) && !control(98,2) && !control(6,64));
    require(controls.channel(3).coarseTuning == 24);
    require(control(101,127) && control(100,127) && !control(6,64));
    controls.reset();
    require(!controls.channel(3).softPedal);
    require(controls.channel(3).coarseTuning == 0 && controls.channel(3).rpnMsb == 127);
    decoder.reset();
    const uint8_t cc[] {0xb3,7,42,10,0,11,3,91,23,93,12};
    decoder.push(cc,[&](auto event) { require(controls.apply(event)); });
    require(controls.channel(3).volume == 42 && controls.channel(3).pan == 1);
    require(controls.channel(3).expression == 3 && controls.channel(3).reverb == 23);
    require(controls.channel(3).chorus == 12 && controls.channel(2).volume == 100);
    require(!controls.apply({Kind::message,0xb3,64,127,2}));
    require(!controls.apply({Kind::message,0x93,60,100,2}));
    controls.reset();
    require(controls.channel(3).pan == 64 && controls.channel(3).expression == 127);
    const uint8_t program[] {0xc3,16};
    decoder.push(program,[&](auto event) { require(controls.apply(event)); });
    require(sc55::ResolveV121MelodicPreset(0, controls.channel(3).program) == 20);
    require(!sc55::ResolveV121MelodicPreset(1,16));
    require(!sc55::ResolveV121MelodicPreset(0,128));
    for (unsigned channel = 0; channel < 16; ++channel)
        for (unsigned value = 0; value < 128; ++value)
        {
            controls.reset(); decoder.reset();
            const std::array<uint8_t,11> bytes{uint8_t(0xb0|channel),7,uint8_t(value),
                11,uint8_t(127-value),10,uint8_t(value),91,uint8_t(value),93,uint8_t(127-value)};
            for (const auto byte : bytes)
                decoder.push(std::span(&byte,1),[&](auto event) { require(controls.apply(event)); });
            sc55::LevelInputs level;
            level.master = 77; level.bias = -123; level.mod1_a = 456;
            level.has_tone_scale = true; level.tone_scale = 64;
            sc55::SpatialInputs spatial;
            spatial.basePan = 17; spatial.masterPan = 23;
            spatial.hasToneScale = true; spatial.panScale = 55;
            spatial.reverbScale = 13; spatial.chorusScale = 19;
            sc55::ApplyChannelOutputControls(controls.channel(channel),level,spatial);
            require(level.velocity == value && level.expression == 127-value
                && spatial.pan == (value == 0 ? 1 : value)
                && spatial.reverb == 127-value && spatial.chorus == value);
            require(level.master == 77 && level.bias == -123 && level.mod1_a == 456
                && level.has_tone_scale && level.tone_scale == 64);
            require(spatial.basePan == 17 && spatial.masterPan == 23 && spatial.hasToneScale
                && spatial.panScale == 55 && spatial.reverbScale == 13 && spatial.chorusScale == 19);
            require(controls.channel((channel+1)%16).volume == 100
                && controls.channel((channel+1)%16).expression == 127);
        }
    std::puts("MIDI channel-to-output controls: 2048 cases, isolated channels and preserved non-channel state");
    std::puts("MIDI fragmentation, running status, realtime, abort and bulk SysEx passed");
    std::puts("Timed MIDI queue: bounded backpressure, atomic SysEx abort/restart, deferral and failure passed");
}
