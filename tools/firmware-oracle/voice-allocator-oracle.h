#pragma once
#include "sc55_voice_allocator.h"
#include "sc55_note_dispatch.h"
#include "sc55_note_start.h"
#include "sc55_envelope_pcm.h"
#include "sc55_voice_lifecycle.h"
#include "sc55_pitch.h"
#include "sc55_lfo.h"
#include "sc55_control_clock.h"
#include "sc55_rhythm.h"
#include "sc55_rhythm_admission.h"
#include "pcm.h"
#include <cstring>
#include <memory>

// One mapping shared by snapshot and verification; never used by native code.
template<class Visit>
void visitAllocatorBytes(sc55::VoiceAllocator& state, Visit&& visit)
{
    const auto array = [&](unsigned base, auto& values) {
        for (unsigned i = 0; i < values.size(); ++i) visit(base+i,values[i]);
    };
    array(0xa318,state.voicePart); array(0xa330,state.voiceGroup);
    array(0xa348,state.status); array(0xa360,state.fieldA360); array(0xa3e0,state.fieldA3E0);
    array(0xa378,state.freeNext); array(0xa390,state.groups.next); array(0xa3a8,state.groups.previous);
    array(0xa2a0,state.groups.head); array(0xa2b8,state.groups.tail);
    array(0xcac4,state.pcmLinks.first); array(0xcadc,state.pcmLinks.second);
    array(0xa240,state.groupNext); array(0xa258,state.groupPrevious);
    array(0xa270,state.groupStatus); array(0xa2e8,state.groupValue);
    array(0xa288,state.groupFieldA288); array(0xa2d0,state.groupFieldA2D0); array(0xa300,state.groupFieldA300);
    array(0xa210,state.partHead); array(0xa220,state.partTail);
    array(0xa1e0,state.partMinimum); array(0xa1f0,state.partVoiceCount);
    array(0xa230,state.partFlags); array(0xa200,state.partPrevious);
    array(0xac42,state.activity); visit(0xa3c0,state.shortage);
    visit(0xa3c1,state.freeCount); visit(0xa3c2,state.freeGroupHead);
    visit(0xa3c3,state.freeHead); visit(0xa3c4,state.freeTail);
}

inline void verifyVoiceAllocator(mcu_t& cpu)
{
    const auto execute = [&](uint16_t start,uint16_t end,uint16_t alternateEnd = 0xffff,unsigned limit = 2000) {
        cpu.cp = cpu.dp = cpu.ep = 0; cpu.pc = start; cpu.sr = 0;
        unsigned instructions = 0;
        while (cpu.pc != end && cpu.pc != alternateEnd)
        {
            if (++instructions > limit) throw std::runtime_error("Voice allocation primitive escaped");
            const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
        }
    };
    const auto write = [&](sc55::VoiceAllocator& state) {
        visitAllocatorBytes(state,[&](unsigned a,uint8_t v) { MCU_Write(cpu,a,v); });
    };
    const auto compare = [&](sc55::VoiceAllocator& state) {
        visitAllocatorBytes(state,[&](unsigned a,uint8_t v) {
            if (v != MCU_Read(cpu,a)) throw std::runtime_error("Voice allocation mismatch at " + std::to_string(a));
        });
    };
    unsigned takeChecks = 0, attachChecks = 0;
    unsigned rhythmKeyChecks = 0;
    for (unsigned mapIndex = 0; mapIndex < 2; ++mapIndex)
        for (unsigned key = 0; key < 128; ++key)
            for (unsigned part = 0; part < 16; ++part)
                for (unsigned seed = 0; seed < 8; ++seed)
                    for (unsigned master : {0u,1u,64u,127u})
                    {
                        sc55::RhythmKeyMap map;
                        map.pitches[key] = uint8_t(key*17+seed*31);
                        const uint8_t shift = uint8_t(seed*37), offset = uint8_t(part*19+seed);
                        const unsigned mapKey = 0x8748+mapIndex*0x48c+key;
                        MCU_Write16(cpu,0xa1c0,0x9400); MCU_Write16(cpu,0xa1c6,uint16_t(mapKey));
                        MCU_Write(cpu,mapKey+0x180,map.pitches[key]);
                        MCU_Write(cpu,0x9406,shift); MCU_Write(cpu,0xab46+part,offset);
                        MCU_Write(cpu,0x8005,uint8_t(master)); MCU_Write(cpu,0xa1b2,uint8_t(key));
                        cpu.r[3] = uint16_t(part); cpu.r[7] = 0x9900;
                        execute(0x11fe,0x1215);
                        const auto native = sc55::PrepareRhythmInitialKeys(key,map,shift,offset);
                        if (!native || uint8_t(cpu.r[0]) != native->initialKey
                            || MCU_Read(cpu,0xa1b3) != native->initialKey || MCU_Read(cpu,0xa1b4) != native->sourceKey
                            || MCU_Read(cpu,0xa1b2) != key || cpu.r[7] != 0x9900)
                            throw std::runtime_error("Rhythm initial keys differ");
                        ++rhythmKeyChecks;
                    }
    std::printf("Native rhythm initial keys: %u H8 cases matched\n",rhythmKeyChecks);
    {
        // Bank2's name-first records end at288b0, before a zero-filled region.
        // The ROM's reachable drum maps include global tone385=224+161.
        // This import deliberately excludes adjacent data and instructions.
        auto bank2 = std::make_unique<SC55PatchTable>();
        if (!bank2->loadRecords(std::span(cpu.rom2).subspan(0x20000,162*216),0,162)
            || bank2->size() != 162 || (*bank2)[0].name != "Glasses"
            || (*bank2)[161].name != "Open Hi Hat2")
            throw std::runtime_error("Bank2 patch boundary changed");
        unsigned maxTone = 0, references = 0;
        for (unsigned program = 0; program < 128; ++program)
        {
            const unsigned set = MCU_Read(cpu,0x38000+program);
            if (set == 255) continue;
            for (unsigned key = 0; key < 128; ++key)
            {
                const unsigned at = 0x38080+set*0x48c+key*2;
                const unsigned tone = MCU_Read(cpu,at)*256+MCU_Read(cpu,at+1);
                if (tone&0x8000) continue;
                if (tone >= 224+162) throw std::runtime_error("Drum map outside imported patch banks");
                maxTone = std::max(maxTone,tone); ++references;
            }
        }
        if (maxTone != 385) throw std::runtime_error("Last bank2 record no longer reachable from rhythm maps");
        for (unsigned index = 0; index < 162; ++index)
        {
            // fbe starts with the real bank-relative patch address multiply.
            // Stop before velocity side effects; validate the imported fields
            // at the actual addresses consumed by that firmware entry.
            const auto& patch = (*bank2)[index];
            const unsigned at = 0x20000+index*216;
            const auto stack = cpu.r[7];
            cpu.cp = cpu.dp = 0; cpu.ep = 2; cpu.pc = 0x0fbe; cpu.sr = 0;
            cpu.r[4] = index; cpu.r[7] = 0x9900;
            unsigned instructions = 0;
            while (cpu.pc != 0x0fd3)
            {
                if (++instructions > 10) throw std::runtime_error("Bank2 patch addressing escaped");
                const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
            }
            if (cpu.r[5] != index*216+32 || uint8_t(cpu.r[0]) != patch.common[6]
                || MCU_Read(cpu,0xa1c2)*256+MCU_Read(cpu,0xa1c3) != index*216)
                throw std::runtime_error("Bank2 H8 patch addressing mismatch");
            cpu.r[7] = stack;
            for (unsigned i = 0; i < 20; ++i)
                if (patch.common[i] != MCU_Read(cpu,at+12+i))
                    throw std::runtime_error("Bank2 common data mismatch");
            for (unsigned p = 0; p < 2; ++p)
                for (unsigned i = 0; i < 92; ++i)
                    if (patch.partial[p].raw[i] != MCU_Read(cpu,at+32+p*92+i))
                        throw std::runtime_error("Bank2 partial data mismatch");
        }
        std::printf("Bank2 data import: 162 records, %u rhythm-map references bounded through tone385\n",references);
    }
    {
        std::array<uint8_t,128> accumulators;
        for (unsigned key = 0; key < 128; ++key) accumulators[key] = MCU_Read(cpu,0x3d168+key);
        unsigned present = 0, absent = 0;
        for (unsigned tone = 0; tone < 65536; ++tone)
            for (unsigned mapIndex = 0; mapIndex < 2; ++mapIndex)
                for (unsigned program : {0u,127u})
                {
                    const unsigned key = tone%128;
                    sc55::RhythmKeyMap map;
                    map.tones[key] = uint16_t(tone);
                    map.groups[key] = uint8_t(tone*73); map.flags[key] = uint8_t(tone/128);
                    const uint8_t accumulator = uint8_t(tone*31);
                    const unsigned base = 0x8748+mapIndex*0x48c;
                    MCU_Write16(cpu,base+2*key,uint16_t(tone));
                    MCU_Write(cpu,base+0x200+key,map.groups[key]);
                    MCU_Write(cpu,base+0x400+key,map.flags[key]);
                    MCU_Write(cpu,0x8049,uint8_t(program));
                    MCU_Write(cpu,0xa1b7,accumulator); MCU_Write(cpu,0xa1d2,0x5a);
                    cpu.r[0] = mapIndex; cpu.r[1] = 0x8000|key; cpu.r[2] = 0x8048;
                    execute(0x0c3c,0x0c9f,0x0ccc);
                    const auto native = sc55::SelectRhythmTone(key,program,map,accumulators,accumulator);
                    if (!native || MCU_Read(cpu,0xa1b6) != native->key
                        || MCU_Read(cpu,0xa1b7) != native->velocityAccumulator
                        || MCU_Read(cpu,0xa1bf) != native->flags || MCU_Read(cpu,0xa3d1) != native->group
                        || MCU_Read(cpu,0xa1c6)*256+MCU_Read(cpu,0xa1c7) != base+key
                        || native->present() != (cpu.pc == 0x0c9f) || cpu.dp != 0)
                        throw std::runtime_error("Native rhythm tone mapping differs from H8");
                    if (native->present())
                    {
                        if (cpu.r[4] != native->bankTone() || cpu.ep != native->bank()
                            || MCU_Read(cpu,0xa1d2) != native->bank())
                            throw std::runtime_error("Native rhythm patch bank differs from H8");
                        ++present;
                    }
                    else
                    {
                        if (cpu.r[4] != tone || MCU_Read(cpu,0xa1d2) != 0x5a)
                            throw std::runtime_error("Absent rhythm tone changed bank state");
                        ++absent;
                    }
                }
        std::printf("Native rhythm tone mapping: %u present/%u absent H8 cases matched\n",present,absent);
    }
    {
        const auto stack = cpu.r[7];
        std::array<uint16_t,24> bases;
        for (unsigned slot = 0; slot < 24; ++slot)
            bases[slot] = uint16_t(MCU_Read(cpu,0x676a+slot*2)*256+MCU_Read(cpu,0x676b+slot*2));
        cpu.r[0] = bases[0]; MCU_Write(cpu,bases[0]-26,0);
        execute(0x3190,0x3195);
        if (MCU_Read(cpu,bases[0]-26) != 255) throw std::runtime_error("H8 visited-entry write differs");
        const auto slotOf = [&](uint16_t base) {
            for (unsigned slot = 0; slot < 24; ++slot) if (bases[slot] == base) return slot;
            throw std::runtime_error("Unknown periodic voice pointer");
        };
        for (unsigned seed = 0; seed < 512; ++seed)
        {
            std::array<uint16_t,24> stages;
            sc55::VoiceLinks links;
            for (unsigned slot = 0; slot < 24; ++slot)
            {
                stages[slot] = uint16_t((seed+slot*7)%24);
                if ((seed+slot)%3 == 0) links.first[slot] = uint8_t((slot+seed)%24);
                if ((seed+slot)%3 == 1) links.second[slot] = uint8_t((slot+7)%24);
                MCU_Write16(cpu,bases[slot],stages[slot]); MCU_Write(cpu,bases[slot]-26,0);
                MCU_Write(cpu,0xcac4+slot,links.first[slot]); MCU_Write(cpu,0xcadc+slot,links.second[slot]);
            }
            std::vector<unsigned> expected,actual;
            sc55::PeriodicVoiceUpdatePass pass;
            for (unsigned steps = 0;; ++steps)
            {
                if (steps > 24) throw std::runtime_error("Native periodic pass escaped");
                const auto result = pass.step(stages,links,
                    [&](const auto& selection) { expected.push_back(0x100+selection.slots[0]);
                        if (selection.count == 2) expected.push_back(0x200+selection.slots[1]); return true; },
                    [&](unsigned slot) { expected.push_back(0x300+slot); return true; },
                    [&](unsigned destination,unsigned) { expected.push_back(0x400+destination); return true; },
                    [&](unsigned slot) { expected.push_back(0x500+slot);
                        using Outcome = sc55::PeriodicVoiceUpdatePass::UpdateResult;
                        return seed >= 256 && (seed+slot)%4 == 0 ? Outcome::skipRemaining : Outcome::proceed; },
                    [&](unsigned slot) { expected.push_back(0x600+slot); return true; });
                if (result == sc55::PeriodicVoiceUpdatePass::Result::complete) break;
                if (result != sc55::PeriodicVoiceUpdatePass::Result::updated) throw std::runtime_error("Native pass rejected fixture");
            }
            cpu.cp = cpu.dp = cpu.ep = 0; cpu.pc = 0x5b0e; cpu.sr = 0; cpu.r[7] = 0xa600;
            unsigned instructions = 0;
            while (cpu.pc != 0x5942)
            {
                if (++instructions > 20000) throw std::runtime_error("H8 periodic pass escaped");
                unsigned event = 0;
                switch (cpu.pc) {
                    case 0x5c20: event = 0x100; break;
                    case 0x5ff5: event = 0x200; break;
                    case 0x3985: event = 0x300; break;
                    case 0x3d44: event = 0x400; break;
                    case 0x3188: event = 0x500; MCU_Write(cpu,cpu.r[0]-26,255); break;
                    case 0x5855: event = 0x600; break;
                }
                if (event)
                {
                    actual.push_back(event+slotOf(cpu.r[0]));
                    // Exercise the real nonlocal termination tail, not an RTS.
                    if (event == 0x500 && seed >= 256 && (seed+slotOf(cpu.r[0]))%4 == 0)
                    {
                        cpu.pc = 0x33e7;
                        continue;
                    }
                    cpu.pc = uint16_t(MCU_Read(cpu,cpu.r[7])*256+MCU_Read(cpu,cpu.r[7]+1)); cpu.r[7] += 2;
                }
                else { const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode); }
            }
            if (actual != expected) throw std::runtime_error("Periodic delegate order differs from H8");
            for (unsigned slot = 0; slot < 24; ++slot)
                if (MCU_Read(cpu,bases[slot]-26) != pass.visited()[slot]) throw std::runtime_error("Periodic pass completion differs");
        }
        cpu.r[7] = stack;
        std::puts("Native periodic pass: 512 H8 orchestration traces matched (normal and nonlocal termination; DSP delegates stubbed)");
    }
    {
        const auto stack = cpu.r[7];
        for (unsigned seed = 0; seed < 1024; ++seed)
        {
            sc55::VoiceInstallationState installed;
            sc55::PartControllerState parts;
            std::array<sc55::VoiceControllerState,24> controllers{};
            const auto first = uint8_t(seed%24), second = uint8_t(seed%5 == 0 ? first : (first+7)%24);
            const sc55::VoiceUpdateSelection selection{{first,second},uint8_t(seed&1 ? 2 : 1),255};
            for (unsigned slot = 0; slot < 24; ++slot)
            {
                auto& input = installed.voices[slot].input;
                input.part = uint8_t((slot+seed)%16); input.originalKey = uint8_t((slot*7+seed)%128);
                MCU_Write(cpu,0xc8e4+slot,input.part); MCU_Write(cpu,0xc8fc+slot,input.originalKey);
            }
            for (unsigned part = 0; part < 16; ++part)
            {
                for (unsigned k = 0; k < 128; ++k)
                {
                    parts.parts[part].keyPressure[k] = uint8_t((seed+part+k)%128);
                    MCU_Write(cpu,0x9740+part*128+k,parts.parts[part].keyPressure[k]);
                }
                for (unsigned i = 0; i < 11; ++i)
                {
                    parts.parts[part].sensitivity[i] = uint8_t(seed+part+i*17);
                    MCU_Write(cpu,0x8048+part*0x70+0x4c+i+(i >= 3),parts.parts[part].sensitivity[i]);
                    for (unsigned source = 0; source < 5; ++source)
                    {
                        const auto value = uint16_t(seed*13+part*31+i*7+source*101);
                        parts.parts[part].contributions[source][i] = value;
                        MCU_Write16(cpu,0x9060+source*0x160+i*0x20+part*2,value);
                    }
                }
            }
            if (!sc55::RefreshSelectedVoiceControllers(selection,installed,parts,controllers))
                throw std::runtime_error("Native controller phase rejected fixture");
            for (auto& r : cpu.r) r = 0;
            cpu.r[7] = 0xa800; cpu.r[0] = 0xa600; cpu.r[1] = first;
            cpu.r[2] = second == first ? 0xa600 : 0xa900; cpu.r[3] = second;
            execute(selection.count == 1 ? 0x5b73 : 0x5bb4,selection.count == 1 ? 0x5b89 : 0x5bdb);
            for (unsigned member = 0; member < selection.count; ++member)
            {
                const auto base = member == 0 || second == first ? 0xa600 : 0xa900;
                const auto& c = controllers[selection.slots[member]];
                const std::array<uint16_t,11> values{c.pitchOffset,c.envelopeOffset,c.levelBias,
                    c.rateModifiers[0],c.pitchDepths[0],c.envelopeDepths[0],c.levelDepths[0],
                    c.rateModifiers[1],c.pitchDepths[1],c.envelopeDepths[1],c.levelDepths[1]};
                const std::array<int,11> offsets{0x86,0x88,0x8a,-0x72,0x90,0x8c,0x8e,-0x50,0x92,0x94,0x96};
                for (unsigned i = 0; i < 11; ++i)
                    if (unsigned(MCU_Read(cpu,base+offsets[i]))*256+MCU_Read(cpu,base+offsets[i]+1) != values[i])
                        throw std::runtime_error("Native selected controller phase differs from H8");
            }
        }
        cpu.r[7] = stack;
        std::puts("Native selected controllers: 1024 complete single/paired H8 phases matched");
    }
    {
        for (unsigned seed = 0; seed < 2048; ++seed)
        {
            std::array<uint16_t,24> stages;
            std::array<uint8_t,24> visited;
            sc55::VoiceLinks links;
            for (unsigned slot = 0; slot < 24; ++slot)
            {
                stages[slot] = uint16_t((seed+slot*7)%26);
                visited[slot] = uint8_t(seed%7 == 0 ? 1 : (seed+slot)%4 == 0);
                if ((seed+slot)%3 == 0) links.first[slot] = uint8_t((seed+slot)%24);
                if ((seed+slot)%3 == 1 || seed%5 == 0) links.second[slot] = uint8_t((seed+slot+7)%24);
                const auto base = unsigned(MCU_Read(cpu,0x676a+slot*2))*256+MCU_Read(cpu,0x676b+slot*2);
                MCU_Write16(cpu,base,stages[slot]); MCU_Write(cpu,base-26,visited[slot]);
                MCU_Write(cpu,0xcac4+slot,links.first[slot]); MCU_Write(cpu,0xcadc+slot,links.second[slot]);
            }
            const auto start = uint8_t(seed%25 == 24 ? 255 : seed%25);
            const auto expected = sc55::SelectNextVoiceUpdate(start,stages,visited,links);
            if (!expected) throw std::runtime_error("Native periodic selector rejected fixture");
            cpu.cp = cpu.dp = cpu.ep = 0; cpu.pc = start == 255 ? 0x5b5c : 0x5b11;
            cpu.sr = 0; cpu.r[1] = start == 255 ? 0 : start;
            unsigned steps = 0;
            while (cpu.pc != 0x5b73 && cpu.pc != 0x5bb4 && cpu.pc != 0x5942)
            {
                if (++steps > 2000) throw std::runtime_error("Periodic selector escaped");
                const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
            }
            const unsigned count = cpu.pc == 0x5942 ? 0 : cpu.pc == 0x5b73 ? 1 : 2;
            if (count != expected->count || (count && cpu.r[1] != expected->slots[0])
                || (count == 2 && cpu.r[3] != expected->slots[1]))
                throw std::runtime_error("Native periodic selection differs from H8, seed "+std::to_string(seed));
            if (count)
            {
                const auto scanned = unsigned(MCU_Read(cpu,0xcb48))*256+MCU_Read(cpu,0xcb49);
                if (expected->resume != uint8_t(scanned == 0 ? 255 : scanned-1))
                    throw std::runtime_error("Native periodic resume differs from H8");
            }
            for (unsigned slot = 0; slot < 24; ++slot)
            {
                const auto base = unsigned(MCU_Read(cpu,0x676a+slot*2))*256+MCU_Read(cpu,0x676b+slot*2);
                if (MCU_Read(cpu,base-26) != visited[slot]) throw std::runtime_error("Native periodic visited reset differs");
            }
        }
        std::puts("Native periodic selector: 2048 H8 scans matched, pair order/resume/visited reset checked");
    }
    {
        const auto stack = cpu.r[7];
        for (unsigned seed = 0; seed < 16384; ++seed)
        {
            sc55::PartControllerState state;
            std::array<sc55::PartMidiReceive,16> routing;
            const uint8_t channel = uint8_t(seed%16);
            const unsigned bend = seed*4051%16384; // visits every fourteen-bit value
            for (unsigned part = 0; part < 16; ++part)
            {
                const unsigned base = 0x8048+part*0x70;
                routing[part] = {uint8_t((seed+part)%3 ? channel : (channel+1)%16),
                    uint16_t((seed+part)&1 ? 0x4000 : 0x3fff),0};
                MCU_Write(cpu,base+4,routing[part].channel); MCU_Write16(cpu,base+2,routing[part].flags);
                for (unsigned i = 0; i < 11; ++i)
                {
                    const auto parameter = uint8_t(seed/16+part*17+i*31);
                    state.parts[part].sourceSensitivity[1][i] = parameter;
                    MCU_Write(cpu,base+0x34+i+(i >= 3),parameter);
                    for (unsigned source = 0; source < 5; ++source)
                    {
                        const auto initial = uint16_t(seed*31+part+source*103+i);
                        state.parts[part].contributions[source][i] = initial;
                        MCU_Write16(cpu,0x9060+source*0x160+i*0x20+part*2,initial);
                    }
                }
            }
            const sc55::MidiDecoder::Event event{sc55::MidiDecoder::Kind::message,uint8_t(0xe0|channel),
                uint8_t(bend&127),uint8_t(bend>>7),2};
            if (!state.receivePitchBend(event,routing)) throw std::runtime_error("Native bend rejected fixture");
            for (auto& r : cpu.r) r = 0;
            cpu.r[7] = 0xa800; cpu.r[0] = event.second; cpu.r[5] = event.first; cpu.r[3] = channel;
            execute(0x2390,0x23f7,0xffff,8000);
            for (unsigned part = 0; part < 16; ++part)
                for (unsigned source = 0; source < 5; ++source)
                    for (unsigned i = 0; i < 11; ++i)
                    {
                        const auto address = 0x9060+source*0x160+i*0x20+part*2;
                        const auto actual = uint16_t(MCU_Read(cpu,address)*256+MCU_Read(cpu,address+1));
                        if (actual != state.parts[part].contributions[source][i])
                            throw std::runtime_error("Native bend differs from H8, seed "+std::to_string(seed));
                    }
        }
        cpu.r[7] = stack;
        std::puts("Native pitch bend: all 16384 input values matched H8 routing and eleven-word contributions");
    }
    {
        const auto stack = cpu.r[7];
        for (unsigned seed = 0; seed < 2048; ++seed)
        {
            sc55::PartControllerState state;
            std::array<sc55::PartMidiReceive,16> routing;
            const uint8_t channel = uint8_t(seed%16), cc = uint8_t(seed%4 ? seed%128 : 1);
            const uint8_t value = uint8_t((seed/4)%128);
            for (unsigned part = 0; part < 16; ++part)
            {
                const unsigned base = 0x8048+part*0x70;
                routing[part] = {uint8_t((seed+part)%5 ? channel : (channel+1)%16),
                    uint16_t((seed+part)%4 == 0 ? 2 : (seed+part)%4 == 1 ? 0x800 : 0x802),0};
                MCU_Write(cpu,base+4,routing[part].channel); MCU_Write16(cpu,base+2,routing[part].flags);
                for (unsigned a = 0; a < 2; ++a)
                {
                    state.parts[part].assignedControllers[a] = uint8_t((seed+part+a)%3 ? cc : (cc+1)%128);
                    MCU_Write(cpu,base+0x26+a,state.parts[part].assignedControllers[a]);
                }
                constexpr std::array<unsigned,5> parameterOffsets{0x28,0x34,0x40,0x58,0x64};
                for (unsigned source = 0; source < 5; ++source)
                    for (unsigned i = 0; i < 11; ++i)
                    {
                        const auto parameter = uint8_t(seed+part*17+source*31+i*11);
                        state.parts[part].sourceSensitivity[source][i] = parameter;
                        MCU_Write(cpu,base+parameterOffsets[source]+i+(i >= 3),parameter);
                        const auto initial = uint16_t(seed*31+part+source*103+i);
                        state.parts[part].contributions[source][i] = initial;
                        MCU_Write16(cpu,0x9060+source*0x160+i*0x20+part*2,initial);
                    }
            }
            const sc55::MidiDecoder::Event event{sc55::MidiDecoder::Kind::message,uint8_t(0xb0|channel),cc,value,2};
            if (!state.receiveControlContributions(event,routing)) throw std::runtime_error("Native CC contributions rejected fixture");
            MCU_Write(cpu,0xac22,cc); MCU_Write(cpu,0xac23,value);
            for (auto& r : cpu.r) r = 0;
            cpu.r[7] = 0xa800; cpu.r[0] = value; cpu.r[3] = channel;
            cpu.r[1] = 15; cpu.r[2] = 0x86d8;
            execute(cc == 1 ? 0x25ac : 0x22e9,0x22f6,0xffff,8000);
            for (unsigned part = 0; part < 16; ++part)
                for (unsigned source = 0; source < 5; ++source)
                    for (unsigned i = 0; i < 11; ++i)
                    {
                        const auto address = 0x9060+source*0x160+i*0x20+part*2;
                        const auto actual = uint16_t(MCU_Read(cpu,address)*256+MCU_Read(cpu,address+1));
                        if (actual != state.parts[part].contributions[source][i])
                            throw std::runtime_error("Native CC contributions differ from H8, seed "+std::to_string(seed));
                    }
        }
        cpu.r[7] = stack;
        std::puts("Native CC contributions: 2048 H8 modulation/assignment cases matched, all five rows checked");
    }
    {
        const auto stack = cpu.r[7];
        for (unsigned seed = 0; seed < 2048; ++seed)
        {
            sc55::PartControllerState state;
            std::array<sc55::PartMidiReceive,16> routing;
            const uint8_t channel = uint8_t(seed%16), pressure = uint8_t(seed%128);
            for (unsigned part = 0; part < 16; ++part)
            {
                routing[part] = {uint8_t((seed+part)%3 ? channel : (channel+1)%16),
                    uint16_t((seed+part)&1 ? 0x2000 : 0x0fff),0};
                MCU_Write(cpu,0x8048+part*0x70+4,routing[part].channel);
                MCU_Write16(cpu,0x8048+part*0x70+2,routing[part].flags);
                for (unsigned i = 0; i < 11; ++i)
                {
                    const auto parameter = uint8_t(seed/8+part*17+i*31);
                    state.parts[part].sourceSensitivity[2][i] = parameter;
                    MCU_Write(cpu,0x8048+part*0x70+0x40+i+(i >= 3),parameter);
                    for (unsigned source = 0; source < 5; ++source)
                    {
                        const auto value = uint16_t(seed*31+part*13+i+source*103);
                        state.parts[part].contributions[source][i] = value;
                        MCU_Write16(cpu,0x9060+source*0x160+i*0x20+part*2,value);
                    }
                }
            }
            const sc55::MidiDecoder::Event event{sc55::MidiDecoder::Kind::message,uint8_t(0xd0|channel),pressure,0,1};
            if (!state.receiveChannelPressure(event,routing)) throw std::runtime_error("Native channel pressure rejected fixture");
            for (auto& r : cpu.r) r = 0;
            cpu.r[7] = 0xa800; cpu.r[0] = pressure; cpu.r[3] = channel;
            execute(0x2346,0x2378);
            for (unsigned part = 0; part < 16; ++part)
                for (unsigned source = 0; source < 5; ++source)
                    for (unsigned i = 0; i < 11; ++i)
                    {
                        const auto address = 0x9060+source*0x160+i*0x20+part*2;
                        const auto actual = uint16_t(MCU_Read(cpu,address)*256+MCU_Read(cpu,address+1));
                        if (actual != state.parts[part].contributions[source][i])
                            throw std::runtime_error("Native channel pressure differs from H8, seed "+std::to_string(seed));
                    }
        }
        cpu.r[7] = stack;
        std::puts("Native channel pressure: 2048 H8 routed eleven-word updates matched, all five rows checked");
    }
    {
        const auto stack = cpu.r[7];
        for (unsigned seed = 0; seed < 2048; ++seed)
        {
            sc55::PartControllerState state;
            std::array<sc55::PartMidiReceive,16> routing;
            const uint8_t channel = uint8_t(seed%16), key = uint8_t((seed/16)%128), pressure = uint8_t(seed*13%128);
            for (unsigned part = 0; part < 16; ++part)
            {
                routing[part] = {uint8_t((seed+part)%3 ? channel : (channel+1)%16),uint16_t((seed+part)&1 ? 0x400 : 0),0};
                MCU_Write(cpu,0x8048+part*0x70+4,routing[part].channel);
                MCU_Write16(cpu,0x8048+part*0x70+2,routing[part].flags);
                for (unsigned k = 0; k < 128; ++k)
                {
                    state.parts[part].keyPressure[k] = uint8_t((seed+part+k)%128);
                    MCU_Write(cpu,0x9740+part*128+k,state.parts[part].keyPressure[k]);
                }
            }
            const sc55::MidiDecoder::Event event{sc55::MidiDecoder::Kind::message,uint8_t(0xa0|channel),key,pressure,2};
            if (!state.receivePolyPressure(event,routing)) throw std::runtime_error("Native pressure rejected fixture");
            for (auto& r : cpu.r) r = 0;
            cpu.r[7] = 0xa800; cpu.r[0] = key; cpu.r[3] = channel; cpu.r[4] = pressure;
            execute(0x2218,0x223b);
            for (unsigned part = 0; part < 16; ++part)
                for (unsigned k = 0; k < 128; ++k)
                    if (MCU_Read(cpu,0x9740+part*128+k) != state.parts[part].keyPressure[k])
                        throw std::runtime_error("Native poly pressure routing differs from H8");
        }
        cpu.r[7] = stack;
        std::puts("Native poly pressure: 2048 H8 routed table updates matched, all keys/parts checked");
    }
    {
        const auto stack = cpu.r[7];
        for (unsigned seed = 0; seed < 4096; ++seed)
        {
            const unsigned part = seed%16, slot = seed%24, key = (seed/16)%128;
            const unsigned partBase = (MCU_Read(cpu,0x74a4+part*2)<<8)|MCU_Read(cpu,0x74a5+part*2);
            sc55::VoiceControllerInputs input; input.keyValue = uint8_t(seed*13);
            uint32_t random = seed+1;
            const auto next = [&] { random = random*1664525u+1013904223u; return uint16_t(random>>16); };
            for (unsigned i = 0; i < 11; ++i)
            {
                input.sensitivity[i] = uint8_t(seed+i*31);
                MCU_Write(cpu,partBase+0x4c+i+(i >= 3),input.sensitivity[i]);
                for (unsigned source = 0; source < 5; ++source)
                {
                    input.contributions[source][i] = seed%3 == 0 ? uint16_t(int(next()%1024)-512) : next();
                    MCU_Write16(cpu,0x9060+source*0x160+i*0x20+part*2,input.contributions[source][i]);
                }
            }
            MCU_Write(cpu,0x9740+part*128+key,input.keyValue);
            MCU_Write(cpu,0xc8e4+slot,uint8_t(part)); MCU_Write(cpu,0xc8fc+slot,uint8_t(key));
            for (auto& r : cpu.r) r = 0;
            cpu.r[7] = 0xa800; cpu.r[0] = 0xa600; cpu.r[1] = uint16_t(slot);
            execute(0x5c20,0x5ff4);
            const auto result = sc55::PrepareVoiceControllers(input);
            const std::array<int,11> offsets{0x86,0x88,0x8a,-0x72,0x90,0x8c,0x8e,-0x50,0x92,0x94,0x96};
            const std::array<uint16_t,11> values{result.pitchOffset,result.envelopeOffset,result.levelBias,
                result.rateModifiers[0],result.pitchDepths[0],result.envelopeDepths[0],result.levelDepths[0],
                result.rateModifiers[1],result.pitchDepths[1],result.envelopeDepths[1],result.levelDepths[1]};
            for (unsigned i = 0; i < 11; ++i)
                if (unsigned(MCU_Read(cpu,0xa600+offsets[i]))*256+MCU_Read(cpu,0xa601+offsets[i]) != values[i])
                    throw std::runtime_error("Native controller preparation differs at field "+std::to_string(i)+" seed "+std::to_string(seed));
        }
        cpu.r[7] = stack;
        std::puts("Native controller preparation: 4096 complete H8 eleven-field cases matched");
    }
    {
        const auto stack = cpu.r[7];
        for (unsigned seed = 0; seed < 2048; ++seed)
        {
            const unsigned source = 0x9200, destination = seed&1 ? source : 0x9600;
            sc55::VoiceControllerState input, output;
            const auto visit = [](auto& state,auto&& fn) {
                fn(0x86,state.pitchOffset); fn(0x8a,state.levelBias); fn(0x88,state.envelopeOffset);
                fn(-0x50,state.rateModifiers[1]); fn(-0x72,state.rateModifiers[0]);
                fn(0x96,state.levelDepths[1]); fn(0x8e,state.levelDepths[0]);
                fn(0x94,state.envelopeDepths[1]); fn(0x8c,state.envelopeDepths[0]);
                fn(0x92,state.pitchDepths[1]); fn(0x90,state.pitchDepths[0]);
            };
            std::array<uint8_t,512> expected;
            for (unsigned i = 0; i < expected.size(); ++i)
            { expected[i] = uint8_t(seed+i*7); MCU_Write(cpu,destination-128+i,expected[i]); }
            visit(input,[&](int offset,uint16_t& v) {
                v = uint16_t(seed*73+offset*311);
                MCU_Write16(cpu,uint16_t(source+offset),v);
            });
            // Self-copy starts with the source words in the destination too.
            for (unsigned i = 0; i < expected.size(); ++i) expected[i] = MCU_Read(cpu,destination-128+i);
            sc55::CopyPairedVoiceControllers(output,input);
            visit(output,[&](int offset,uint16_t v) {
                expected[128+offset] = uint8_t(v>>8); expected[129+offset] = uint8_t(v);
            });
            for (auto& r : cpu.r) r = 0;
            cpu.r[7] = 0x9800; cpu.r[0] = uint16_t(destination); cpu.r[2] = uint16_t(source);
            execute(0x5ff5,0x6049);
            for (unsigned i = 0; i < expected.size(); ++i)
                if (MCU_Read(cpu,destination-128+i) != expected[i]) throw std::runtime_error("Paired controller transfer differs");
        }
        cpu.r[7] = stack;
        std::puts("Native paired controllers: 2048 H8 transfers matched, untouched destination bytes checked");
    }
    {
        const auto stack = cpu.r[7];
        for (unsigned seed = 0; seed < 4096; ++seed)
        {
            const unsigned slot = seed%24;
            sc55::InstalledVoice installed{{uint16_t(seed%224),uint16_t(seed%758),uint8_t(seed%2),
                uint8_t(seed%16),60,64,100,uint8_t(seed),uint8_t(seed*17),false},uint8_t(seed/16),2};
            uint8_t activity = uint8_t(seed*13);
            const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
            const auto base = word(0x676a+slot*2);
            const auto patch = 0x10000+installed.input.tone*SC55Patch::SIZE;
            const auto partial = patch+0x20+installed.input.partial*0x5c;
            const auto sample = *sc55::V121SampleDescriptorOffset(installed.input.sample);
            MCU_Write16(cpu,0xc9ec+slot*2,uint16_t(patch)); MCU_Write16(cpu,0xca34+slot*2,uint16_t(partial));
            MCU_Write16(cpu,0xca7c+slot*2,uint16_t(sample));
            MCU_Write(cpu,0xca1c+slot,1); MCU_Write(cpu,0xca64+slot,1); MCU_Write(cpu,0xcaac+slot,uint8_t(sample>>16));
            MCU_Write(cpu,0xc9d4+slot,installed.flags); MCU_Write(cpu,0xc8e4+slot,installed.input.part);
            MCU_Write(cpu,0xcb0c+slot,installed.input.sampleMode); MCU_Write(cpu,0xc92c+slot,installed.input.sampleKey);
            MCU_Write(cpu,0xac42+slot,activity);
            for (auto& r : cpu.r) r = 0;
            cpu.r[7] = 0x9800; cpu.r[1] = uint16_t(slot);
            execute(0x5639,0x56b4);
            const auto result = sc55::PrepareVoiceContext(slot,installed,activity);
            if (!result) throw std::runtime_error("Native preparation context rejected fixture");
            using Table = sc55::VoicePreparationContext::KeyTable;
            const unsigned keyed = result->keyTable == Table::none ? 0
                : (result->keyTable == Table::first ? 0x8748 : 0x8bd4)+result->keyIndex;
            if (word(base-2) != result->slot || MCU_Read(cpu,base+0x9b) != result->part
                || MCU_Read(cpu,base-0x3b) != result->flags || MCU_Read(cpu,0xac42+slot) != activity
                || word(base+0x30) != keyed || word(base+0x2e) != word(0x74a4+result->part*2)
                || word(base+0x9c) != uint16_t(0x10000+result->tone*SC55Patch::SIZE)
                || word(base+0x9e) != uint16_t(0x10000+result->tone*SC55Patch::SIZE+0x20+result->partial*0x5c)
                || word(base+0xa0) != uint16_t(*sc55::V121SampleDescriptorOffset(result->sample))
                || MCU_Read(cpu,base+0x98) != 1 || MCU_Read(cpu,base+0x99) != 1
                || MCU_Read(cpu,base+0x9a) != sample>>16)
                throw std::runtime_error("Native preparation context differs from H8");
        }
        cpu.r[7] = stack;
        std::puts("Native preparation context: 4096 H8 identity/part/key-table/activity cases matched");
    }
    {
        const auto stack = cpu.r[7];
        for (unsigned seed = 0; seed < 3072; ++seed)
        {
            std::array<sc55::VoiceStopState,24> state{};
            sc55::VoiceLinks links; links.first.fill(255); links.second.fill(255);
            std::array<uint8_t,24> activity; activity.fill(93);
            const unsigned slot = seed%24, mode = (seed/24)%4;
            const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
            state[slot].fieldCAF4 = mode == 3 ? 4 : 2;
            if (mode == 1) links.first[slot] = uint8_t((slot+7)%24);
            if (mode == 2) links.second[slot] = uint8_t((slot+11)%24);
            for (unsigned i = 0; i < 24; ++i)
            {
                state[i].stages = {uint16_t((seed/96)%32),3,5};
                MCU_Write(cpu,0xcaf4+i,state[i].fieldCAF4);
                MCU_Write(cpu,0xcac4+i,links.first[i]); MCU_Write(cpu,0xcadc+i,links.second[i]);
                MCU_Write(cpu,0xac42+i,activity[i]);
                const auto base = word(0x676a+i*2);
                for (unsigned j = 0; j < 3; ++j) MCU_Write16(cpu,base+j*2,state[i].stages[j]);
            }
            for (auto& r : cpu.r) r = 0;
            cpu.r[7] = 0x9800; cpu.r[1] = 23;
            execute(0x54bf,mode == 3 ? 0x54b4 : mode == 0 ? 0x55de : 0x5542);
            const auto result = sc55::DispatchNextVoiceTask(state,links,activity);
            if (!result || result->count != (mode == 1 || mode == 2 ? 2 : 1)
                || (mode != 3 && (result->slots[0] != cpu.r[1]
                    || (result->count == 2 && result->slots[1] != cpu.r[2]))))
                throw std::runtime_error("Native task dispatch ordering differs");
            for (unsigned i = 0; i < 24; ++i)
            {
                if (state[i].fieldCAF4 != MCU_Read(cpu,0xcaf4+i) || activity[i] != MCU_Read(cpu,0xac42+i))
                    throw std::runtime_error("Native task dispatch flags/activity differ");
                const auto base = word(0x676a+i*2);
                for (unsigned j = 0; j < 3; ++j)
                    if (state[i].stages[j] != word(base+j*2)) throw std::runtime_error("Native stop task stages differ");
            }
        }
        cpu.r[7] = stack;
        std::puts("Native voice task dispatch: 3072 H8 selection/link/stop cases matched");
    }
    {
        const auto stack = cpu.r[7];
        for (unsigned seed = 0; seed < 2048; ++seed)
        {
            sc55::VoiceAllocator allocator; allocator.initializeTables();
            const unsigned part = seed%16;
            const auto group = allocator.createGroup({uint8_t(part),0x80,60,1,2});
            if (!group) throw std::runtime_error("Installation fixture allocation failed");
            const unsigned slot = group->voices[(seed/4)%2];
            allocator.fieldA3E0[slot] = 173;
            write(allocator);
            sc55::VoiceInstallationState installed;
            installed.pendingRelease.fill(197);
            const sc55::VoiceInstallationInput input{uint16_t(seed%224),
                uint16_t((seed&1) ? 0x8000|(seed%758) : seed%758),uint8_t((seed/2)%2),uint8_t(part),
                uint8_t(seed*3),uint8_t(seed*5),uint8_t(seed*7),uint8_t(seed*11),uint8_t(seed*13),bool(seed&2)};
            const auto sampleOffset = sc55::V121SampleDescriptorOffset(input.sample).value_or(0x12345);
            const auto patchOffset = 0x10000+input.tone*SC55Patch::SIZE;
            const auto partialOffset = patchOffset+0x20+input.partial*0x5c;
            uint8_t flags = uint8_t(seed/8);
            MCU_Write16(cpu,0xa1d0,input.sample);
            MCU_Write16(cpu,0xa1c2,uint16_t(patchOffset)); MCU_Write16(cpu,0xa1c4,uint16_t(partialOffset));
            MCU_Write(cpu,0xa1d2,1); MCU_Write(cpu,0xa1d3,uint8_t(sampleOffset>>16));
            MCU_Write(cpu,0xa3d0,input.part); MCU_Write(cpu,0xa1b2,input.originalKey);
            MCU_Write(cpu,0xa1b3,input.adjustedKey); MCU_Write(cpu,0xa3d3,input.velocity);
            MCU_Write(cpu,0xa1b5,input.sampleMode); MCU_Write(cpu,0xa1b6,input.sampleKey);
            MCU_Write(cpu,0xa1b1,flags); MCU_Write(cpu,0xa1be,input.restarted ? 128 : 0);
            const std::array<unsigned,3> words{0xca7c,0xc9ec,0xca34};
            const std::array<unsigned,11> bytes{0xc8e4,0xc8fc,0xc98c,0xc9d4,0xc914,0xcb0c,
                0xc92c,0xca1c,0xca64,0xcaac,0xcaf4};
            for (auto a : words) MCU_Write16(cpu,a+slot*2,0x5a5a);
            for (auto a : bytes) MCU_Write(cpu,a+slot,0x5a);
            MCU_Write(cpu,0xac2a+slot,197);
            for (auto& r : cpu.r) r = 0;
            cpu.r[7] = 0x9800; cpu.r[1] = uint16_t(slot); cpu.r[5] = uint16_t(sampleOffset);
            execute(0x113a,0x11cf);
            if (!installed.install(slot,input,flags,allocator)) throw std::runtime_error("Native installation rejected fixture");
            compare(allocator);
            if (flags != MCU_Read(cpu,0xa1b1) || installed.pendingRelease[slot] != MCU_Read(cpu,0xac2a+slot))
                throw std::runtime_error("Native installation flags/release differ");
            const bool absent = (input.sample&0x8000) != 0;
            const std::array<uint16_t,3> expectedWords{uint16_t(sampleOffset),uint16_t(patchOffset),uint16_t(partialOffset)};
            const auto& record = installed.voices[slot];
            if (!absent && !(record.input == input)) throw std::runtime_error("Native installed identity differs");
            const std::array<uint8_t,11> expectedBytes{record.input.part,record.input.originalKey,record.input.adjustedKey,
                record.flags,record.input.velocity,record.input.sampleMode,record.input.sampleKey,1,1,
                uint8_t(sampleOffset>>16),record.taskState};
            for (unsigned i = 0; i < words.size(); ++i)
                if (unsigned(MCU_Read(cpu,words[i]+slot*2))*256+MCU_Read(cpu,words[i]+slot*2+1)
                    != (absent ? 0x5a5a : expectedWords[i])) throw std::runtime_error("Installation data identity differs");
            for (unsigned i = 0; i < bytes.size(); ++i)
                if (MCU_Read(cpu,bytes[i]+slot) != (absent ? 0x5a : expectedBytes[i]))
                    throw std::runtime_error("Native installed metadata differs");
        }
        cpu.r[7] = stack;
        std::puts("Native voice installation: 2048 H8 cases matched, including 1024 negative-sample returns");
    }
    {
        const auto stack = cpu.r[7];
        for (unsigned seed = 0; seed < 2048; ++seed)
        {
            SC55Patch patch;
            const uint8_t candidates = uint8_t(seed);
            std::array<uint8_t,2> slots{uint8_t((seed/16)%24),uint8_t((seed/32)%24)};
            if (seed&4) patch.partial[0].raw[2] = patch.partial[0].raw[3] = 255;
            if (seed&8) patch.partial[1].raw[2] = patch.partial[1].raw[3] = 255;
            if (seed&64) slots[0] = 255;
            if (seed&128) slots[1] = 255;
            MCU_Write(cpu,0xa1b0,candidates); MCU_Write(cpu,0xa1b1,0);
            MCU_Write(cpu,0xa3d0,uint8_t(seed%16)); MCU_Write(cpu,0xa3d6,slots[0]); MCU_Write(cpu,0xa3d7,slots[1]);
            MCU_Write(cpu,0xa1c2,0x90); MCU_Write(cpu,0xa1c3,0);
            for (unsigned i = 0; i < 2; ++i)
            {
                const unsigned base = 0x9020+i*0x5c;
                MCU_Write(cpu,base+2,patch.partial[i].raw[2]); MCU_Write(cpu,base+3,patch.partial[i].raw[3]);
            }
            std::array<sc55::PartialVoiceDispatch,2> observed{};
            sc55::PartialDispatchState staged;
            for (unsigned part = 0; part < 16; ++part)
                for (unsigned i = 0; i < 2; ++i)
                {
                    staged.previousKeys[part][i] = uint8_t(seed+part*3+i);
                    MCU_Write(cpu,0xa190+part*2+i,staged.previousKeys[part][i]);
                }
            const auto visitInputs = [&](auto&& visit) {
                for (unsigned slot = 0; slot < 24; ++slot)
                {
                    const auto& value = staged.voices[slot];
                    visit(0xc9a4+slot*2,uint8_t(value.pitchFraction>>8));
                    visit(0xc9a5+slot*2,uint8_t(value.pitchFraction));
                    visit(0xc944+slot,value.amplitude); visit(0xc95c+slot,value.secondary);
                    visit(0xc974+slot,value.previousKey);
                }
            };
            for (unsigned slot = 0; slot < 24; ++slot)
                staged.voices[slot] = {uint16_t(seed*31+slot),uint8_t(seed+slot),uint8_t(seed-slot),uint8_t(slot+128)};
            visitInputs([&](unsigned a,uint8_t v) { MCU_Write(cpu,a,v); });
            const uint8_t adjusted = uint8_t(seed*17);
            MCU_Write(cpu,0xa1b3,adjusted);
            MCU_Write16(cpu,0xa1ca,uint16_t(seed*71));
            for (unsigned i = 0; i < 2; ++i)
            {
                MCU_Write(cpu,0xa1b9+i,uint8_t(seed*13+i));
                MCU_Write(cpu,0xa1bc+i,uint8_t(seed*19+i));
                MCU_Write(cpu,0xa1cc+i,uint8_t(seed*23+i));
            }
            for (auto& reg : cpu.r) reg = 0;
            cpu.cp = cpu.dp = cpu.ep = 0; cpu.pc = 0x1219; cpu.sr = 0; cpu.r[7] = 0x9800;
            unsigned instructions = 0;
            while (cpu.pc != 0x132a)
            {
                if (++instructions > 500) throw std::runtime_error("Partial dispatch escaped");
                // Stub delegated sample preparation/installation; this check
                // proves dispatch only, not the behavior of those routines.
                if (cpu.pc == 0x123a || cpu.pc == 0x12bb)
                { observed[cpu.pc == 0x123a ? 0 : 1].prepare = true; cpu.pc += 3; continue; }
                if (cpu.pc == 0x1241 || cpu.pc == 0x12c2) { cpu.pc += 3; continue; }
                if (cpu.pc == 0x129b || cpu.pc == 0x1323)
                { observed[cpu.pc == 0x129b ? 0 : 1].voice = uint8_t(cpu.r[1]); cpu.pc += 3; continue; }
                const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
            }
            const auto plan = sc55::PlanPartialVoiceDispatch(patch,candidates,slots);
            if (!plan) throw std::runtime_error("Native partial dispatch rejected fixture");
            for (unsigned i = 0; i < 2; ++i)
            {
                if ((*plan)[i].prepare != observed[i].prepare
                    || ((*plan)[i].voice < 128 ? (*plan)[i].voice : 255) != observed[i].voice)
                    throw std::runtime_error("Native partial dispatch differs from H8");
                if (!staged.stage(seed%16,i,(*plan)[i],adjusted,
                    {uint16_t(seed*71),uint8_t(seed*13+i),uint8_t(seed*19+i),uint8_t(seed*23+i)}))
                    throw std::runtime_error("Native partial staging rejected fixture");
            }
            for (unsigned part = 0; part < 16; ++part)
                for (unsigned i = 0; i < 2; ++i)
                    if (staged.previousKeys[part][i] != MCU_Read(cpu,0xa190+part*2+i))
                        throw std::runtime_error("Native previous partial key differs from H8");
            visitInputs([&](unsigned a,uint8_t v) {
                if (MCU_Read(cpu,a) != v) throw std::runtime_error("Native partial slot input differs from H8");
            });
        }
        cpu.r[7] = stack;
        std::puts("Native partial dispatch/staging: 2048 H8 cases matched (delegated calls stubbed)");
    }
    {
        for (unsigned seed = 0; seed < 4096; ++seed)
        {
            sc55::VoiceAllocator allocator; allocator.initializeTables();
            const auto part = uint8_t(seed%16);
            const auto created = allocator.createGroup({part,0x80,60,1,2});
            if (!created) throw std::runtime_error("Mono reuse fixture allocation failed");
            const auto tail = allocator.groups.tail[created->group], head = allocator.groups.head[created->group];
            allocator.status[tail] = uint8_t(seed/16);
            allocator.status[head] = uint8_t((seed/16)*73);
            if (seed&1) allocator.groups.head[created->group] = tail;
            if (seed%5 == 0) allocator.groups.head[created->group] = 255;
            const sc55::VoiceAllocator::MonoReuse input{uint8_t(seed),{uint8_t(seed+11),uint8_t(seed+37)}};
            write(allocator);
            MCU_Write(cpu,0xa1b1,input.flags); MCU_Write(cpu,0xa3d6,input.voices[0]); MCU_Write(cpu,0xa3d7,input.voices[1]);
            cpu.r[3] = part; execute(0x0b1a,0x0b64);
            const auto result = allocator.prepareMonoReuse(part,input);
            if (!result || result->flags != MCU_Read(cpu,0xa1b1)
                || result->voices[0] != MCU_Read(cpu,0xa3d6) || result->voices[1] != MCU_Read(cpu,0xa3d7))
                throw std::runtime_error("Native mono reuse selection differs from H8");
            compare(allocator);
        }
        std::puts("Native mono reuse: 4096 H8 slot/flag selections matched");
    }
    {
        const auto stack = cpu.r[7];
        std::array<unsigned,3> branches{};
        for (unsigned seed = 0; seed < 4096; ++seed)
        {
            sc55::MonoHeldKeys keys;
            const unsigned part = seed%16, key = (seed/16)%128, base = 0x9f40+part*16;
            const auto current = uint8_t(seed&1 ? key : (key+1)%128);
            keys.set(key,true);
            if (seed&2) keys.set((key+13)%128,true);
            if (seed&4) keys.set((key+7)%128,true);
            for (unsigned i = 0; i < 8; ++i)
            { MCU_Write(cpu,base+2*i,uint8_t(keys.words[i]>>8)); MCU_Write(cpu,base+2*i+1,uint8_t(keys.words[i])); }
            MCU_Write(cpu,0xa070+part,current);
            cpu.cp = cpu.dp = cpu.ep = 0; cpu.pc = 0x0a64; cpu.sr = 0;
            cpu.r[1] = key; cpu.r[3] = part; cpu.r[7] = 0x9000;
            unsigned instructions = 0;
            while (cpu.pc != 0x0aec && cpu.pc != 0x0aab && cpu.pc != 0x0a74)
            {
                if (++instructions > 500) throw std::runtime_error("Mono release decision escaped");
                const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
            }
            const auto result = keys.release(key,current);
            if (!result) throw std::runtime_error("Native mono release decision rejected valid key");
            using Action = sc55::MonoHeldKeys::ReleaseDecision::Action;
            const auto expected = cpu.pc == 0x0aec ? Action::unchangedVoice
                : cpu.pc == 0x0aab ? Action::releaseGroup : Action::replaceKey;
            if (result->action != expected || (expected == Action::replaceKey && result->replacement != cpu.r[1]))
                throw std::runtime_error("Native mono release decision differs from H8");
            ++branches[unsigned(expected)];
            for (unsigned i = 0; i < 8; ++i)
                if (keys.words[i] != unsigned(MCU_Read(cpu,base+2*i))*256+MCU_Read(cpu,base+2*i+1))
                    throw std::runtime_error("Mono release decision bitmap differs from H8");
            if (MCU_Read(cpu,0xa070+part) != current) throw std::runtime_error("Mono decision changed current key");
        }
        cpu.r[7] = stack;
        for (auto count : branches) if (!count) throw std::runtime_error("Missing mono decision branch coverage");
        std::printf("Native mono release decision: 4096 H8 cases matched (%u unchanged, %u release, %u replace)\n",
            branches[0],branches[1],branches[2]);
    }
    {
        for (unsigned seed = 0; seed < 2048; ++seed)
        {
            sc55::VoiceAllocator allocator; allocator.initializeTables();
            const auto part = uint8_t(seed%16);
            const auto created = allocator.createGroup({part,0x80,60,1,uint8_t(1+(seed/16)%2)});
            if (!created) throw std::runtime_error("Mono release fixture allocation failed");
            allocator.partFlags[part] = uint8_t(seed/32);
            allocator.groupStatus[created->group] = uint8_t(seed/4);
            allocator.groupFieldA288[created->group] = uint8_t(seed*13);
            // Distinguish head from previous: mono release must use head.
            allocator.groups.previous[created->voices[0]] = 255;
            if (seed%7 == 0) allocator.groups.tail[created->group] = 255;
            if (seed%11 == 0) allocator.groups.head[created->group] = allocator.groups.tail[created->group];
            write(allocator);
            cpu.r[1] = 0xffff; cpu.r[2] = 0; cpu.r[3] = part;
            execute(0x0aab,0x0aec);
            if (!allocator.releaseMonoGroup(part)) throw std::runtime_error("Native mono release rejected fixture");
            compare(allocator);
        }
        std::puts("Native mono group release: 2048 H8 cases matched");
    }
    {
        for (unsigned seed = 0; seed < 2048; ++seed)
        {
            sc55::MonoHeldKeys keys;
            const unsigned part = seed/128, key = seed%128, base = 0x9f40+part*16;
            if (seed&1) keys.words.fill(0xffff);
            const auto compareKeys = [&] {
                for (unsigned i = 0; i < 8; ++i)
                    if (keys.words[i] != unsigned(MCU_Read(cpu,base+2*i))*256+MCU_Read(cpu,base+2*i+1))
                        throw std::runtime_error("Native mono key bitmap differs from H8");
            };
            for (unsigned i = 0; i < 8; ++i)
            { MCU_Write(cpu,base+2*i,uint8_t(keys.words[i]>>8)); MCU_Write(cpu,base+2*i+1,uint8_t(keys.words[i])); }
            for (bool down : {true,true,false,false})
            {
                cpu.r[1] = key; cpu.r[3] = part;
                execute(down ? 0x0984 : 0x09a0,down ? 0x099f : 0x09bb);
                if (!keys.set(key,down)) throw std::runtime_error("Native mono key rejected valid key");
                compareKeys();
                cpu.r[3] = part; execute(0x09bc,0x09f6);
                const auto highest = keys.highest();
                if (cpu.r[1] != (highest ? unsigned(*highest) : 0xffffu))
                    throw std::runtime_error("Native highest held key differs from H8");
            }
            // Nontrivial masks and every singleton cover all word/bit scan positions.
            for (unsigned i = 0; i < 8; ++i)
                keys.words[i] = (seed&128) ? uint16_t((seed*73+i*977)^(seed<<i)) : 0;
            if (!(seed&128)) keys.set(key,true);
            for (unsigned i = 0; i < 8; ++i)
            { MCU_Write(cpu,base+2*i,uint8_t(keys.words[i]>>8)); MCU_Write(cpu,base+2*i+1,uint8_t(keys.words[i])); }
            cpu.r[3] = part; execute(0x09bc,0x09f6);
            const auto highest = keys.highest();
            if (cpu.r[1] != (highest ? unsigned(*highest) : 0xffffu))
                throw std::runtime_error("Native highest key mask scan differs from H8");
            compareKeys();
        }
        std::puts("Native mono held keys: 8192 H8 bit updates and 10240 highest-key scans matched");
    }
    {
        const auto stack = cpu.r[7];
        unsigned groups = 0, mono = 0;
        for (unsigned flags = 0; flags < 256; ++flags)
            for (unsigned key = 0; key < 128; ++key)
            {
                MCU_Write(cpu,0x804d,uint8_t(flags));
                cpu.r[1] = key; cpu.r[2] = 0x8048; cpu.r[5] = key; cpu.r[7] = 0x9000;
                execute(0x0a19,0x155a,0x0a64);
                const auto selected = sc55::SelectNoteRelease(key,uint8_t(flags));
                if (!selected) throw std::runtime_error("Native release selector rejected valid key");
                const bool isGroup = selected->path == sc55::NoteReleaseSelection::Path::group;
                if (isGroup != (cpu.pc == 0x155a)
                    || (isGroup && selected->selector != uint8_t(cpu.r[0])) || cpu.r[1] != key)
                    throw std::runtime_error("Native note release selector differs from H8");
                isGroup ? ++groups : ++mono;
            }
        cpu.r[7] = stack;
        std::printf("Native release selector: 32768 H8 cases matched (%u group, %u mono)\n",groups,mono);
    }
    {
        unsigned accepted = 0;
        for (unsigned seed = 0; seed < 32768; ++seed)
        {
            const unsigned part = seed%16, channel = (seed/16)%16;
            const sc55::PartMidiReceive receive{uint8_t((seed&256) ? channel : (channel+1)%16),
                uint16_t((seed&512) ? 0xffff : 0xfdff)};
            const sc55::NoteReceiveMode mode{uint8_t((seed/1024)%16),
                uint8_t((seed&16384) ? part : (part+1)%16),uint8_t(seed/8192)};
            constexpr unsigned base = 0x8048;
            MCU_Write(cpu,base+4,receive.channel);
            MCU_Write(cpu,base+2,uint8_t(receive.flags>>8)); MCU_Write(cpu,base+3,uint8_t(receive.flags));
            MCU_Write(cpu,0xcdcc,mode.fieldCDCC); MCU_Write(cpu,0xcdf4,mode.fieldCDF4); MCU_Write(cpu,0xcdf5,mode.fieldCDF5);
            cpu.r[1] = part; cpu.r[2] = base; cpu.r[3] = channel;
            execute(0x2076,0x209c,0x209e);
            const auto native = sc55::AcceptNotePart(channel,part,receive,mode);
            if (native != (cpu.pc == 0x209c)) throw std::runtime_error("Native note receive gate differs from H8");
            accepted += native;
            cpu.r[1] = part; cpu.r[2] = base; cpu.r[3] = channel;
            execute(0x20fe,0x2126,0x2191);
            if (native != (cpu.pc == 0x2126)) throw std::runtime_error("Native note-on receive gate differs from H8");
        }
        if (!accepted || accepted == 32768) throw std::runtime_error("Note receive fixture lacks branch coverage");
        std::printf("Native note receive: 32768 paired on/off gate cases matched (%u accepted)\n",accepted);
    }
    {
        unsigned accepted = 0, cases = 0;
        for (unsigned seed = 0; seed < 65536; ++seed)
        {
            const unsigned part = seed%16;
            const uint8_t bypass = uint8_t(seed/16);
            const sc55::NoteKeyRange range{uint8_t(seed/256),uint8_t(seed*73)};
            MCU_Write(cpu,0xac0e,bypass);
            MCU_Write(cpu,0xac0f,0xff); // Adjacent byte must not grant bypass.
            MCU_Write(cpu,0x8054,range.low); MCU_Write(cpu,0x8055,range.high);
            for (unsigned key : {0u,127u,unsigned(range.low&127),unsigned(range.high&127),
                unsigned((range.low-1)&127),unsigned((range.high+1)&127)})
            {
                cpu.r[1] = part; cpu.r[2] = 0x8048; cpu.r[4] = key;
                execute(0x2150,0x2160,0x2191);
                const bool native = sc55::AcceptNoteKeyRange(part,key,range,bypass);
                if (native != (cpu.pc == 0x2160) || cpu.r[1] != part || cpu.r[4] != key)
                    throw std::runtime_error("Native note key range differs from H8");
                ++cases; accepted += native;
            }
        }
        if (!accepted || accepted == cases) throw std::runtime_error("Key range lacks branch coverage");
        std::printf("Native note key range: %u H8 cases matched (%u accepted)\n",cases,accepted);
    }
    {
        const auto stack = cpu.r[7];
        unsigned cases = 0, zero = 0;
        for (unsigned depth = 0; depth < 128; ++depth)
            for (unsigned offset = 0; offset < 128; ++offset)
            {
                MCU_Write(cpu,0x8052,uint8_t(depth)); MCU_Write(cpu,0x8053,uint8_t(offset));
                for (unsigned velocity = 1; velocity < 128; ++velocity)
                {
                    cpu.r[0] = velocity; cpu.r[1] = 0x5678; cpu.r[2] = 0x8048;
                    cpu.r[4] = 60; cpu.r[7] = 0x9000;
                    execute(0x21be,0x2207);
                    const auto native = sc55::AdjustPartNoteVelocity(velocity,{uint8_t(depth),uint8_t(offset)});
                    if (!native || *native != uint8_t(cpu.r[0]) || cpu.r[1] != 0x5678
                        || cpu.r[2] != 0x8048 || cpu.r[4] != 60 || cpu.r[7] != 0x9000)
                        throw std::runtime_error("Native part velocity adjustment differs from H8");
                    ++cases; zero += *native == 0;
                }
            }
        cpu.r[7] = stack;
        if (!zero) throw std::runtime_error("Part velocity fixture lacks zero rejection");
        std::printf("Native part velocity: %u H8 cases matched (%u zero results)\n",cases,zero);
    }
    {
        const auto stack = cpu.r[7];
        std::array<unsigned,6> outcomes{};
        unsigned cases = 0;
        for (unsigned seed = 0; seed < 32768; ++seed)
            for (unsigned config = 0; config < 4; ++config)
                for (unsigned velocity : {0u,1u,64u,127u})
                {
                    const unsigned key = seed/256, part = key%16;
                    sc55::PartNoteOnInputs input;
                    input.noteFlags = uint8_t(seed);
                    input.rhythmKeyFlags = {uint8_t(seed/8),uint8_t(~(seed/8))};
                    if (config == 1) { input.velocityAdjustment = {1,64}; input.keyRange = {127,0}; }
                    if (config == 2) { input.velocityAdjustment = {63,65}; input.keyRange = {60,72}; }
                    if (config == 3) { input.velocityAdjustment = {127,0}; input.fieldAC0E = uint8_t(seed*73); }
                    MCU_Write(cpu,0x804d,input.noteFlags);
                    MCU_Write(cpu,0x8052,input.velocityAdjustment.depth);
                    MCU_Write(cpu,0x8053,input.velocityAdjustment.offset);
                    MCU_Write(cpu,0x8054,input.keyRange.low); MCU_Write(cpu,0x8055,input.keyRange.high);
                    MCU_Write(cpu,0xac0e,input.fieldAC0E);
                    MCU_Write(cpu,0x8b48+key,input.rhythmKeyFlags[0]);
                    MCU_Write(cpu,0x8fd4+key,input.rhythmKeyFlags[1]);
                    cpu.cp = cpu.dp = cpu.ep = 0; cpu.pc = 0x2126; cpu.sr = 0;
                    cpu.r[0] = velocity; cpu.r[1] = part; cpu.r[2] = 0x8048;
                    cpu.r[4] = key; cpu.r[7] = 0x9900;
                    unsigned instructions = 0;
                    while (cpu.pc != 0x2160 && cpu.pc != 0x218e && cpu.pc != 0x2191)
                    {
                        if (++instructions > 100) throw std::runtime_error("Part Note On receive escaped");
                        const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                    }
                    const auto native = sc55::SelectPartNoteOn(part,key,velocity,input);
                    using Status = sc55::PartNoteOnSelection::Status;
                    const auto end = native.status == Status::prepare ? 0x2160
                        : native.status == Status::release ? 0x218e : 0x2191;
                    if (native.status == Status::invalidInput || cpu.pc != end
                        || (native.status == Status::prepare && uint8_t(cpu.r[0]) != native.velocity)
                        || cpu.r[1] != part || cpu.r[2] != 0x8048 || cpu.r[4] != key || cpu.r[7] != 0x9900)
                        throw std::runtime_error("Native part Note On receive differs from H8");
                    ++outcomes[unsigned(native.status)]; ++cases;
                }
        cpu.r[7] = stack;
        for (unsigned i = 1; i < outcomes.size(); ++i)
            if (!outcomes[i]) throw std::runtime_error("Part Note On receive missing outcome coverage");
        std::printf("Native part Note On receive: %u H8 cases matched (release/rhythm/velocity/key/prepare %u/%u/%u/%u/%u)\n",
            cases,outcomes[1],outcomes[2],outcomes[3],outcomes[4],outcomes[5]);
    }
    {
        for (unsigned seed = 0; seed < 2048; ++seed)
        {
            sc55::VoiceAllocator allocator; allocator.initializeTables();
            const auto part = uint8_t(seed%16);
            for (unsigned i = 0; i < 8; ++i)
            {
                const auto created = allocator.createGroup({part,0,uint8_t(40+i),1,uint8_t(1+i%2)});
                if (!created) throw std::runtime_error("Retained release fixture allocation failed");
                allocator.groupStatus[created->group] = (seed+i)%3 == 0 ? 0 : 2;
                allocator.groupFieldA288[created->group] = uint8_t(seed>>(i%5));
            }
            std::array<uint8_t,16> retained; retained.fill(255);
            for (unsigned i = 0; i < (seed/16)%17; ++i) retained[i] = uint8_t(40+(i+seed)%16);
            write(allocator);
            for (unsigned i = 0; i < 16; ++i) MCU_Write(cpu,0xa090+16*part+i,retained[i]);
            cpu.cp = 4; cpu.dp = cpu.ep = 0; cpu.pc = 0x0640; cpu.sr = 0; cpu.r[3] = part;
            unsigned instructions = 0;
            while (cpu.pc != 0x06b5)
            {
                if (++instructions > 10000) throw std::runtime_error("Retained release escaped");
                const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
            }
            if (!allocator.releaseRetainedKeys(part,retained))
                throw std::runtime_error("Native retained release rejected fixture");
            for (unsigned i = 0; i < 16; ++i)
                if (retained[i] != MCU_Read(cpu,0xa090+16*part+i))
                    throw std::runtime_error("Retained release row differs from H8");
            compare(allocator);
        }
        std::puts("Native retained-key release: 2048 H8 cases matched");
    }
    {
        for (unsigned seed = 0; seed < 2048; ++seed)
        {
            sc55::VoiceAllocator allocator; allocator.initializeTables();
            const unsigned part = seed%16;
            const unsigned count = (seed/16)%25;
            for (unsigned i = 0; i < count; ++i)
            {
                const auto created = allocator.createGroup({uint8_t(part),0,
                    uint8_t(40+(i*(1+seed%5))%19),1,1});
                if (!created) throw std::runtime_error("Retained-key fixture allocation failed");
                allocator.groupStatus[created->group] = (seed+i)%7 == 0 ? 2 : 0;
            }
            std::array<uint8_t,16> retained; retained.fill(255);
            for (unsigned i = 0; i < (seed/400)%17; ++i) retained[i] = uint8_t(40+i);
            if (seed%11 == 0) retained.fill(60);
            write(allocator);
            for (unsigned i = 0; i < 16; ++i) MCU_Write(cpu,0xa090+16*part+i,retained[i]);
            cpu.cp = 4; cpu.dp = cpu.ep = 0; cpu.pc = 0x05f1; cpu.sr = 0; cpu.r[3] = part;
            unsigned instructions = 0;
            while (cpu.pc != 0x063e)
            {
                if (++instructions > 10000) throw std::runtime_error("Retained-key capture escaped");
                const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
            }
            if (!allocator.captureRetainedKeys(part,retained))
                throw std::runtime_error("Native retained-key capture rejected fixture");
            for (unsigned i = 0; i < 16; ++i)
                if (retained[i] != MCU_Read(cpu,0xa090+16*part+i))
                    throw std::runtime_error("Retained-key capture differs from H8");
            compare(allocator);
        }
        std::puts("Native retained-key capture: 2048 H8 cases matched");
    }
    {
        for (unsigned seed = 0; seed < 2048; ++seed)
        {
            sc55::VoiceAllocator allocator; allocator.initializeTables();
            const auto part = uint8_t(seed%16);
            for (unsigned i = 0; i < 5; ++i)
            {
                const auto created = allocator.createGroup({part,0,uint8_t(60+i),1,uint8_t(1+i%2)});
                if (!created) throw std::runtime_error("Hold fixture allocation failed");
                allocator.groupFieldA288[created->group] = uint8_t(seed>>(i%4));
                allocator.groupStatus[created->group] = uint8_t(seed+i);
            }
            allocator.partFlags[part] = uint8_t(seed/16);
            std::array<uint8_t,16> retained; retained.fill(255);
            if (seed&1) { retained[0] = 60; retained[1] = 62; }
            if (seed%7 == 0) retained.fill(64);
            write(allocator);
            for (unsigned i = 0; i < 16; ++i) MCU_Write(cpu,0xa090+16*part+i,retained[i]);
            cpu.r[3] = part;
            execute(0x16d2,0x1736);
            if (!allocator.setPartHold(part,false,retained)) throw std::runtime_error("Native hold-off rejected fixture");
            compare(allocator);
            cpu.r[3] = part; execute(0x16cd,0x16d1);
            if (!allocator.setPartHold(part,true,retained)) throw std::runtime_error("Native hold-on rejected fixture");
            compare(allocator);
        }
        std::puts("Native part hold: 2048 H8 hold-off/on pairs matched");
    }
    {
        const auto stack = cpu.r[7];
        unsigned matched = 0;
        for (unsigned seed = 0; seed < 2048; ++seed)
        {
            sc55::VoiceAllocator allocator;
            allocator.initializeTables();
            const auto part = uint8_t(seed%16), note = uint8_t(60+(seed/16)%3);
            const auto selector = uint8_t((seed/48)%3);
            for (unsigned i = 0; i < 4; ++i)
            {
                const auto created = allocator.createGroup({part,uint8_t(i%3),uint8_t(60+i%2),
                    uint8_t((seed>>(i+3))&3),2});
                if (!created) throw std::runtime_error("Release fixture allocation failed");
                allocator.groupStatus[created->group] = ((seed>>(i+7))&1) ? 2 : 0;
                allocator.groupFieldA288[created->group] = uint8_t(seed*7)&0xfe;
            }
            allocator.partFlags[part] = uint8_t(seed/128);
            std::array<uint8_t,16> retained;
            retained.fill(255);
            if (seed&1) { retained[0] = 60; retained[1] = 61; }
            if (seed%7 == 0) retained.fill(62);
            write(allocator);
            for (unsigned i = 0; i < 16; ++i) MCU_Write(cpu,0xa090+16*part+i,retained[i]);
            cpu.r[0] = selector; cpu.r[1] = note; cpu.r[3] = part; cpu.r[7] = 0x9000;
            execute(0x155a,0x1597);
            const auto result = allocator.requestNoteRelease(part,note,selector,retained);
            if (!result) throw std::runtime_error("Native note release rejected valid fixture");
            matched += *result;
            compare(allocator);
        }
        cpu.r[7] = stack;
        if (!matched || matched == 2048) throw std::runtime_error("Release fixture lacks matching/nonmatching cases");
        std::printf("Native note-release selection: 2048 H8 cases matched (%u selected)\n",matched);
    }
    {
        for (unsigned seed = 0; seed < 256; ++seed)
        {
            sc55::VoiceAllocator allocator;
            for (unsigned slot = 0; slot < 24; ++slot)
            {
                allocator.fieldA3E0[slot] = uint8_t(seed+slot*17);
                MCU_Write(cpu,0xa3e0+slot,allocator.fieldA3E0[slot]);
                MCU_Write(cpu,0xac2a+slot,uint8_t(~allocator.fieldA3E0[slot]));
            }
            execute(0x1e04,0x1e1a);
            unsigned remaining = 24;
            allocator.publishReleaseRequests([&](uint8_t slot,uint8_t value) {
                if (slot != --remaining || value != MCU_Read(cpu,0xac2a+slot)
                    || value != MCU_Read(cpu,0xa3e0+slot))
                    throw std::runtime_error("Native release publication differs from H8");
            });
            if (remaining != 0) throw std::runtime_error("Incomplete release publication");
        }
        std::puts("Native release publication: 256 H8 batches, 6144 slot writes matched");
    }
    {
        const auto ramControl = cpu.dev_register[DEV_RAMCR];
        cpu.dev_register[DEV_RAMCR] |= 0x80; // Kernel task state is internal RAM.
        for (unsigned expirations = 1; expirations <= 512; ++expirations)
        {
            sc55::ControlTaskClock clock;
            MCU_Write(cpu,0xfe10,0); MCU_Write(cpu,0xfdea,0x35);
            for (unsigned i = 0; i < expirations; ++i)
            {
                cpu.r[2] = 0xfe1a;
                execute(0x03cd,0x03d3); // byte increment + ready flag
            }
            clock.advance(uint64_t(expirations)*160256);
            if (MCU_Read(cpu,0xfe10) != uint8_t(expirations) || MCU_Read(cpu,0xfdea) != 0xb5)
                throw std::runtime_error("Control expiration counter differs");
            MCU_Write16(cpu,0xfdca,8); cpu.r[0] = 0xab00;
            execute(0x02ee,0x02fe); // TRAPA B body, without stack/exception return
            const auto elapsed = clock.consume();
            if (!elapsed || *elapsed != uint8_t(cpu.r[0]) || clock.consume()
                || MCU_Read(cpu,0xfe10) != 0 || MCU_Read(cpu,0xfdea) != 0x35)
                throw std::runtime_error("Native control elapsed consume differs");
            execute(0x5af3,0x5af9);
            if (MCU_Read(cpu,0xac5a) != 0 || MCU_Read(cpu,0xac5b) != *elapsed)
                throw std::runtime_error("Native control elapsed word differs");
        }
        std::puts("Native control-task handoff: 512 H8 cases matched, including byte wrap");
        cpu.dev_register[DEV_RAMCR] = ramControl;
    }
    {
        sc55::PanTable table;
        std::copy_n(cpu.rom1+0x6c8f,table.size(),table.begin());
        for (unsigned position = 0; position <= 128; ++position)
            for (bool frozen : {false,true})
            {
                sc55::SpatialState spatial;
                if (!spatial.initializePan(uint8_t(position),frozen,table)) throw std::runtime_error("Pan initialization rejected");
                cpu.r[0] = 0x9400; cpu.r[3] = uint16_t(position);
                MCU_Write16(cpu,0x9436,frozen ? 65535 : uint16_t(position));
                execute(0x306c,0x3080);
                const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                if (spatial.pan != word(0x9436) || spatial.panWord != word(0x9434)) throw std::runtime_error("Initial pan gains differ from H8");
            }
        std::puts("Native initial pan encoding: 258 H8 cases matched");
        auto panPcm = std::make_unique<pcm_t>();
        PCM_Init(*panPcm,cpu);
        const auto savedPcm = cpu.pcm; cpu.pcm = panPcm.get();
        const auto savedBr = cpu.br; cpu.br = 0xe0;
        for (unsigned i = 0; i < 4096; ++i)
        {
            sc55::SpatialInputs input;
            input.pan = uint8_t(i%128); input.basePan = uint8_t((i*17)%128);
            input.masterPan = uint8_t((i/16)%128); input.hasToneScale = (i&1) != 0;
            input.panScale = uint8_t((i/2)%128);
            input.reverb = uint8_t(i*31); input.chorus = uint8_t(i*13);
            input.reverbScale = uint8_t(i*7); input.chorusScale = uint8_t(i*19);
            MCU_Write16(cpu,0x942e,0x9600); MCU_Write16(cpu,0x9430,input.hasToneScale ? 0x9800 : 0);
            MCU_Write16(cpu,0x9438,input.basePan); MCU_Write(cpu,0x8006,input.masterPan);
            MCU_Write(cpu,0x9609,input.pan); MCU_Write(cpu,0x960e,input.reverb); MCU_Write(cpu,0x960f,input.chorus);
            MCU_Write(cpu,0x9a80,input.panScale); MCU_Write(cpu,0x9b80,input.reverbScale); MCU_Write(cpu,0x9b00,input.chorusScale);
            const auto random = uint16_t(i*313);
            cpu.pcm->ram2[30][10] = random;
            cpu.r[0] = 0x9400;
            execute(0x2fcc,0x3080);
            sc55::SpatialState spatial;
            unsigned reads = 0, writes = 0;
            if (!spatial.initialize(input,table,[&](uint8_t a)->uint8_t {
                    ++reads; return a == 0x3a ? uint8_t(random>>8) : a == 0x3b ? uint8_t(random) : 0;
                },[&](uint8_t a,uint8_t v) {
                    if (a != 0x3e || v != 30) throw std::runtime_error("Unexpected initial pan transaction");
                    ++writes;
                })) throw std::runtime_error("Initial spatial control rejected");
            const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
            const bool randomPan = input.pan == 0 || (input.hasToneScale && input.panScale == 0);
            if (spatial.pan != word(0x9436) || spatial.panWord != word(0x9434) || spatial.effects != word(0x943a)
                || reads != (randomPan ? 3u : 0u) || writes != (randomPan ? 1u : 0u))
                throw std::runtime_error("Initial spatial control differs from H8 at "+std::to_string(i));
        }
        cpu.br = savedBr;
        cpu.pcm = savedPcm;
        std::puts("Native initial spatial control: 4096 H8 cases matched");
    }
    {
        const auto stack = cpu.r[7];
        cpu.r[2] = 0xffff; cpu.r[3] = 0; cpu.r[4] = 0; cpu.r[6] = 32767;
        cpu.r[7] = 0x9000; MCU_Write16(cpu,0x9000,0x7000); execute(0x312b,0x7000);
        if (cpu.r[4] != 0) throw std::runtime_error("H8 amplitude subtraction no longer clamps to zero");
        int32_t level = 0; sc55::ApplyModulation(level,-1,0,32767);
        if (level != cpu.r[4]) throw std::runtime_error("Minimal amplitude subtraction clamp differs");
        for (unsigned edge = 0; edge < 2; ++edge)
        {
            const int16_t a = edge ? 0 : -32768, b = -32768;
            const uint16_t expected = edge ? 17233 : 50000;
            cpu.r[2] = uint16_t(a); cpu.r[3] = uint16_t(b); cpu.r[4] = 50000;
            cpu.r[6] = edge ? 32767 : 12345; cpu.r[7] = 0x9000;
            execute(0x312b,0x7000);
            if (cpu.r[4] != expected) throw std::runtime_error("H8 amplitude signed-edge expectation differs");
            level = 50000; sc55::ApplyModulation(level,a,b,edge ? 32767 : 12345);
            if (level != expected) throw std::runtime_error("Native amplitude signed-edge differs");
        }
        cpu.r[7] = stack;
    }
    {
        const auto stack = cpu.r[7];
        uint32_t seed = 0x312b;
        const auto random = [&]() { seed = seed*1664525u+1013904223u; return uint16_t(seed>>16); };
        sc55::PanTable panTable{};
        for (unsigned i = 0; i < panTable.size(); ++i) panTable[i] = MCU_Read(cpu,0x6c8f+i);
        for (unsigned i = 0; i < 16384; ++i)
        {
            sc55::LevelInputs level;
            level.expression = uint8_t(random()); level.velocity = uint8_t(random()); level.master = uint8_t(random());
            level.has_tone_scale = (i&1) != 0; level.tone_scale = uint8_t(random());
            level.bias = int16_t(random()); level.mod1_b = int16_t(random()); level.mod2_b = int16_t(random());
            sc55::ModulationBlock first,second;
            first.output = {random(),random(),random()}; second.output = {random(),random(),random()};
            first.wave.output = random(); second.wave.output = random();
            if (i < 8) // exact silence shortcuts, even with nonzero modulation
                level.expression = 0;
            sc55::SecondEnvelopeOutputInputs envelope;
            sc55::PitchModulationInputs pitch;
            sc55::ApplyVoiceModulationOutputs(first,second,level,envelope,pitch);
            MCU_Write(cpu,0xc8e4,0); MCU_Write(cpu,0xab36,level.expression);
            MCU_Write16(cpu,0x942e,0x9600); MCU_Write(cpu,0x9608,level.velocity); MCU_Write(cpu,0x8002,level.master);
            MCU_Write16(cpu,0x9430,level.has_tone_scale ? 0x9700 : 0); MCU_Write(cpu,0x9800,level.tone_scale);
            MCU_Write16(cpu,0x948a,uint16_t(level.bias)); MCU_Write16(cpu,0x948e,uint16_t(level.mod1_b));
            MCU_Write16(cpu,0x9496,uint16_t(level.mod2_b));
            MCU_Write16(cpu,0x9386,first.output[0]); MCU_Write16(cpu,0x93a8,second.output[0]);
            MCU_Write16(cpu,0x93a0,first.wave.output); MCU_Write16(cpu,0x93c2,second.wave.output);
            cpu.r[0] = 0x9400; cpu.r[1] = 0; cpu.r[7] = 0x9000;
            MCU_Write16(cpu,0x9000,0x7000); execute(0x309b,0x7000);
            if (sc55::ComputeLevel(level) != cpu.r[5])
                throw std::runtime_error("Native modulation-to-level differs at " + std::to_string(i));
            sc55::TvaState initialTva{random(),random(),random()};
            MCU_Write16(cpu,0x9406,initialTva.ramp);
            MCU_Write16(cpu,0x9418,initialTva.level); MCU_Write16(cpu,0x941a,initialTva.command);
            MCU_Write16(cpu,0x93fe,0);
            cpu.r[0] = 0x9400; cpu.r[7] = 0x9000;
            execute(0x2c5d,0x2c62);
            execute(0x3080,0x308d);
            initialTva.initialize(level);
            const auto initialWord = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
            if (initialTva.ramp != initialWord(0x9406) || initialTva.level != initialWord(0x9418)
                || initialTva.command != initialWord(0x941a))
                throw std::runtime_error("Native initial TVA differs at "+std::to_string(i));
            sc55::VoiceOutputState output;
            output.tva = {random(),random(),random()};
            auto& tva = output.tva;
            output.spatial = {i%17 == 0 ? uint16_t(65535) : uint16_t(random()%129),random(),random()};
            sc55::SpatialInputs spatial;
            spatial.pan = uint8_t(random()%128); spatial.basePan = uint8_t(random()%128);
            spatial.masterPan = uint8_t(random()%128); spatial.panScale = uint8_t(random()%128);
            spatial.reverb = uint8_t(random()); spatial.chorus = uint8_t(random());
            spatial.reverbScale = uint8_t(random()); spatial.chorusScale = uint8_t(random());
            spatial.hasToneScale = level.has_tone_scale;
            MCU_Write16(cpu,0x9436,output.spatial.pan); MCU_Write16(cpu,0x9434,output.spatial.panWord);
            MCU_Write16(cpu,0x943a,output.spatial.effects); MCU_Write16(cpu,0x9438,spatial.basePan);
            MCU_Write(cpu,0x9609,spatial.pan); MCU_Write(cpu,0x8006,spatial.masterPan);
            MCU_Write(cpu,0x960e,spatial.reverb); MCU_Write(cpu,0x960f,spatial.chorus);
            MCU_Write(cpu,0x9980,spatial.panScale); MCU_Write(cpu,0x9a80,spatial.reverbScale);
            MCU_Write(cpu,0x9a00,spatial.chorusScale);
            const uint16_t stage = uint16_t(i%16);
            MCU_Write16(cpu,0x9400,stage); MCU_Write16(cpu,0x93fe,0);
            MCU_Write16(cpu,0x9406,tva.ramp); MCU_Write16(cpu,0x9418,tva.level); MCU_Write16(cpu,0x941a,tva.command);
            cpu.r[0] = 0x9400; cpu.r[7] = 0x9000;
            execute(0x36db,0x7000,0x3363);
            const auto result = output.advance(stage,level,spatial,panTable);
            const auto active = result == sc55::VoiceOutputState::Result::updated;
            const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
            if (active != (cpu.pc == 0x7000) || tva.ramp != word(0x9406)
                || tva.level != word(0x9418) || tva.command != word(0x941a))
                throw std::runtime_error("Native TVA update differs at " + std::to_string(i));
            if (output.spatial.pan != word(0x9436) || output.spatial.panWord != word(0x9434)
                || output.spatial.effects != word(0x943a))
                throw std::runtime_error("Native spatial update differs at " + std::to_string(i));
        }
        cpu.r[7] = stack;
        std::printf("Native modulation-to-level: 16384 H8 cases matched\n");
        std::printf("Native TVA update: 16384 H8 cases matched\n");
        std::printf("Native initial TVA: 16384 H8 cases matched\n");
        std::printf("Native TVA/pan/effect output task: 16384 H8 cases matched\n");
    }
    {
        const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
        std::array<uint16_t,128> depths{}; std::array<uint16_t,256> rates{};
        for (unsigned i = 0; i < 128; ++i) depths[i] = word(0x7312+2*i);
        for (unsigned i = 0; i < 256; ++i) rates[i] = word(0x7012+2*i);
        const sc55::LfoWaveformTables tables;
        const auto stack = cpu.r[7];
        const auto fields = [](const sc55::ModulationBlock& b) {
            return std::array<uint16_t,16>{b.depth[0],b.depth[1],b.depth[2],b.output[0],b.output[1],b.output[2],
                uint16_t(b.rateModifier),b.delayRate,b.attackRate,uint16_t(b.waveform*2),b.wave.phase,
                b.delay,b.attack,b.wave.held,b.wave.smoothed,b.wave.output};
        };
        for (unsigned i = 0; i < 4096; ++i)
        {
            std::array<sc55::FirstModulationVoice,24> voices{};
            for (unsigned v = 0; v < 24; ++v) voices[v].sharing.source = v%2 ? 1 : 24;
            auto& dest = voices[0]; auto& source = voices[1];
            dest.sharing = {1,uint8_t(i*17),uint8_t(i%6 == 5 ? 0 : 255)};
            source.sharing.sharing = uint8_t(i*7);
            source.firstStage = i%6 == 1 ? 14 : 12;
            source.commonBank = i%6 == 2 ? 1 : 0;
            source.field9b = i%6 == 3 ? 1 : 0;
            source.commonIdentity = i%6 == 4 ? 1 : 0;
            dest.block.depth = {uint16_t(i*31),uint16_t(i*47),1234};
            dest.block.rateIndex = 67; dest.block.rateModifier = int16_t(i*13);
            dest.block.delayRate = 1200; dest.block.attackRate = 2300; dest.block.waveform = 3;
            dest.block.wave = {12345,23456,34567,45678}; dest.block.delay = 65534; dest.block.attack = uint16_t(i*71);
            source.block = dest.block; source.block.wave.phase = uint16_t(i*97);
            source.block.wave.output = uint16_t(i*101); source.block.delayRate = 444; source.block.attackRate = 555;
            source.block.waveform = 2; source.block.rateModifier = int16_t(i*29);
            source.block.delay = i%4 == 0 ? 65535 : uint16_t(i*53);
            source.block.attack = (i/4)%2 ? 65535 : uint16_t(i*61);
            for (unsigned v = 0; v < 2; ++v)
            {
                const unsigned a = v ? 0x9600 : 0x9400;
                const auto& voice = voices[v]; const auto values = fields(voice.block);
                for (unsigned k = 0; k < values.size(); ++k) MCU_Write16(cpu,a-(k < 6 ? 0x80 : 0x7e)+2*k,values[k]);
                MCU_Write(cpu,a-0x74,voice.block.rateIndex); MCU_Write(cpu,a-0x73,voice.sharing.sharing);
                MCU_Write(cpu,a-0x19,voice.sharing.baseRate); MCU_Write16(cpu,a,voice.firstStage);
                MCU_Write(cpu,a+0x98,voice.commonBank); MCU_Write(cpu,a+0x9b,voice.field9b);
                MCU_Write16(cpu,a+0x9c,voice.commonIdentity); MCU_Write16(cpu,a-2,uint16_t(v));
            }
            for (unsigned v = 0; v < 24; ++v) MCU_Write16(cpu,0xc84e +2*v,voices[v].sharing.source == 24 ? 0 : 0x9600);
            const uint8_t pitch = uint8_t(i*43), rateControl = uint8_t(i%128), depthControl = uint8_t((i/32)%128);
            MCU_Write16(cpu,0x942e,0x9700); MCU_Write(cpu,0x9710,rateControl); MCU_Write(cpu,0x9711,depthControl);
            MCU_Write16(cpu,0x94a8,pitch); MCU_Write16(cpu,0xac5a,3);
            cpu.r[0] = 0x9400; cpu.r[7] = 0x9000; MCU_Write16(cpu,0x9000,0x7000);
            execute(0x3985,0x7000,0x39e1);
            sc55::FirstVoiceModulationUpdate task;
            using Result = sc55::FirstVoiceModulationUpdate::Result;
            const auto route = task.begin(0,voices,pitch,depthControl,depths);
            const bool detached = cpu.pc == 0x39e1;
            const bool changed = detached && ((i/6)&1);
            if (detached)
            {
                execute(0x39e1,0x39f0);
                if (changed) { ++dest.firstStage; MCU_Write16(cpu,0x9400,dest.firstStage); }
                execute(0x39f0,0x7000);
            }
            if (route == Result::ready)
            {
                const auto result = task.resume(voices,3,pitch,rateControl,depthControl,depths,rates,tables,
                    [](uint8_t)->uint8_t { throw std::runtime_error("Unexpected first modulation PCM read"); },
                    [](uint8_t,uint8_t) { throw std::runtime_error("Unexpected first modulation PCM write"); });
                if (result != (changed ? Result::stageChanged : Result::updated)) throw std::runtime_error("First modulation resume differs");
            }
            else if (route != Result::shared) throw std::runtime_error("First modulation update rejected fixture");
            const auto actual = fields(dest.block);
            for (unsigned k = 0; k < actual.size(); ++k)
                if (actual[k] != word(0x9400-(k < 6 ? 0x80 : 0x7e)+2*k))
                    throw std::runtime_error("First modulation periodic state differs at " + std::to_string(i)+"/"+std::to_string(k));
            for (unsigned v = 0; v < 24; ++v)
            {
                const auto link = voices[v].sharing.source;
                if (word(0xc84e +2*v) != (link == 24 ? 0 : link == 0 ? 0x9400 : 0x9600))
                    throw std::runtime_error("First modulation periodic links differ");
            }
            if (dest.sharing.sharing != MCU_Read(cpu,0x938d) || dest.sharing.baseRate != MCU_Read(cpu,0x93e7)
                || dest.block.rateIndex != MCU_Read(cpu,0x938c)) throw std::runtime_error("First modulation periodic metadata differs");
            // Pair scheduling enters the same tail without source-eligibility
            // checks. Cover it even when the periodic sharing path detached.
            if (!sc55::UpdatePairedFirstModulation(dest,source,pitch,depthControl,depths))
                throw std::runtime_error("Paired first modulation rejected fixture");
            cpu.r[0] = 0x9400; cpu.r[2] = 0x9600; cpu.r[7] = 0x9000;
            MCU_Write16(cpu,0x9000,0x7000);
            execute(0x3d44,0x7000);
            const auto paired = fields(dest.block);
            for (unsigned k = 0; k < paired.size(); ++k)
                if (paired[k] != word(0x9400-(k < 6 ? 0x80 : 0x7e)+2*k))
                    throw std::runtime_error("Paired first modulation differs at "+std::to_string(i)+"/"+std::to_string(k));
            if (dest.sharing.sharing != MCU_Read(cpu,0x938d) || dest.sharing.baseRate != MCU_Read(cpu,0x93e7)
                || dest.block.rateIndex != MCU_Read(cpu,0x938c)) throw std::runtime_error("Paired first modulation metadata differs");
        }
        cpu.r[7] = stack;
        std::printf("Native first modulation periodic task: 4096 H8 cases matched\n");
        std::puts("Native paired first modulation: 4096 complete H8 shared-tail cases matched");
    }
    {
        const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
        std::array<uint16_t,24> addresses{};
        for (unsigned i = 0; i < 24; ++i) addresses[i] = word(0x676a+2*i);
        unsigned checks = 0;
        for (unsigned channel = 0; channel < 24; ++channel)
            for (unsigned candidate = 0; candidate < 24; ++candidate)
                for (unsigned reason = 0; reason < 7; ++reason)
                {
                    std::array<sc55::FirstModulationVoice,24> voices{};
                    for (auto& v : voices) { v.firstStage = 14; v.commonBank = 0; v.commonIdentity = 0x9000; v.field9b = 3; }
                    voices[channel].firstStage = 0;
                    voices[candidate].firstStage = reason == 1 ? 13 : 12;
                    if (reason == 2 && candidate != channel) voices[candidate].field9b = 4;
                    if (reason == 3 && candidate != channel) voices[candidate].commonBank = 1;
                    if (reason == 4 && candidate != channel) voices[candidate].commonIdentity = 0x9100;
                    if (reason == 6) // multiple candidates: descending order, skipping self
                        for (auto& v : voices) v.firstStage = 0;
                    for (unsigned i = 0; i < 24; ++i)
                    {
                        MCU_Write16(cpu,addresses[i],voices[i].firstStage);
                        MCU_Write(cpu,addresses[i]+0x9b,voices[i].field9b);
                        MCU_Write(cpu,addresses[i]+0x98,voices[i].commonBank);
                        MCU_Write16(cpu,addresses[i]+0x9c,voices[i].commonIdentity);
                    }
                    const uint8_t mode = reason == 5 ? 0 : 16;
                    // Start after common pointer/EP setup; synthetic common input
                    // resides in RAM so neither fixture depends on patch contents.
                    MCU_Write(cpu,0x900e,mode); cpu.r[0] = addresses[channel]; cpu.r[5] = 0x9000;
                    execute(0x38a5,0x38e2,0x3d1a);
                    const auto selected = sc55::SelectFirstModulationSource(channel,mode,voices);
                    const auto expected = cpu.pc == 0x38e2 ? uint8_t(24) : uint8_t(cpu.r[1]);
                    if (selected != expected) throw std::runtime_error("First modulation source selection differs");
                    std::array<sc55::VoiceModulation,24> second{};
                    for (unsigned i = 0; i < 24; ++i)
                    {
                        second[i].firstStage = voices[i].firstStage;
                        second[i].field99 = voices[i].commonBank; second[i].field9b = voices[i].field9b;
                        second[i].partialIdentity = voices[i].commonIdentity;
                        MCU_Write(cpu,addresses[i]+0x99,second[i].field99);
                        MCU_Write16(cpu,addresses[i]+0x9e,second[i].partialIdentity);
                        MCU_Write16(cpu,addresses[i]-2,uint16_t(i));
                    }
                    MCU_Write(cpu,0x9004,mode); cpu.r[0] = addresses[channel]; cpu.r[5] = 0x9000;
                    execute(0x2a87,0x2b26,0x2ac2);
                    const auto secondSource = sc55::SelectSecondModulationSource(channel,mode,second);
                    const auto secondExpected = cpu.pc == 0x2b26 ? uint8_t(24) : uint8_t(cpu.r[1]);
                    if (secondSource != secondExpected) throw std::runtime_error("Second modulation source selection differs");
                    if (secondSource < 24)
                    {
                        const auto fields = [](const sc55::ModulationBlock& b) {
                            return std::array<uint16_t,16>{b.depth[0],b.depth[1],b.depth[2],b.output[0],b.output[1],b.output[2],
                                uint16_t(b.rateModifier),b.delayRate,b.attackRate,uint16_t(b.waveform*2),b.wave.phase,
                                b.delay,b.attack,b.wave.held,b.wave.smoothed,b.wave.output};
                        };
                        for (unsigned i = 0; i < 24; ++i)
                        {
                            auto& b = second[i].block;
                            const uint16_t value = uint16_t(checks*31+i*127);
                            b.depth = {value,uint16_t(value+1),uint16_t(value+2)};
                            b.output = {uint16_t(value+3),uint16_t(value+4),uint16_t(value+5)};
                            b.rateModifier = int16_t(value+6); b.delayRate = uint16_t(value+7); b.attackRate = uint16_t(value+8);
                            b.waveform = uint8_t(i%7); b.wave = {uint16_t(value+9),uint16_t(value+10),uint16_t(value+11),uint16_t(value+12)};
                            b.delay = uint16_t(value+13); b.attack = uint16_t(value+14); b.rateIndex = uint8_t(value);
                            const auto values = fields(b);
                            for (unsigned k = 0; k < values.size(); ++k)
                                MCU_Write16(cpu,addresses[i]-(k < 6 ? 0x5e : 0x5c)+2*k,values[k]);
                            MCU_Write(cpu,addresses[i]-0x52,b.rateIndex);
                        }
                        MCU_Write(cpu,addresses[channel]-0x3b,128);
                        MCU_Write(cpu,0x9008,uint8_t(checks));
                        cpu.r[0] = addresses[channel];
                        execute(0x2a66,0x2bac);
                        std::array<uint8_t,24> sources{}; sources.fill(24);
                        SC55Partial partial{}; partial.raw[4] = mode; partial.raw[8] = uint8_t(checks);
                        const std::array<uint16_t,256> unused{};
                        const sc55::LfoWaveformTables tables;
                        unsigned io = 0;
                        sc55::VoiceStopState stop; stop.flagMinus3B = 128;
                        const auto route = sc55::PrepareVoiceSecondModulation(channel,stop,second,sources,partial,unused,unused,tables,
                            [&](uint8_t)->uint8_t { ++io; return 0; },[&](uint8_t,uint8_t) { ++io; });
                        const auto values = fields(second[channel].block);
                        for (unsigned k = 0; k < values.size(); ++k)
                            if (values[k] != word(addresses[channel]-(k < 6 ? 0x5e : 0x5c)+2*k))
                                throw std::runtime_error("Second shared initialization block differs");
                        if (route != sc55::SecondModulationPreparation::shared || io != 0 || sources[channel] != secondSource
                            || second[channel].fieldA2 != MCU_Read(cpu,addresses[channel]+0xa2)
                            || word(0xc87e +2*channel) != addresses[secondSource]
                            || MCU_Read(cpu,addresses[channel]-0x51) != second[channel].sharing
                            || MCU_Read(cpu,addresses[channel]-0x52) != second[channel].block.rateIndex)
                            throw std::runtime_error("Second shared initialization metadata differs");
                    }
                    ++checks;
                }
        std::printf("Native first modulation source selection: %u H8 cases matched\n",checks);
        std::printf("Native second modulation source selection: %u H8 cases matched (1128 shared transfers)\n",checks);
    }
    {
        std::array<uint16_t,128> depths{};
        for (unsigned i = 0; i < 128; ++i) depths[i] = uint16_t((cpu.rom1[0x7312+2*i]<<8)|cpu.rom1[0x7313+2*i]);
        const auto stack = cpu.r[7];
        const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
        uint32_t seed = 0x39a472;
        const auto random = [&]() { seed = seed*1664525u+1013904223u; return uint16_t(seed>>16); };
        for (unsigned i = 0; i < 32768; ++i)
        {
            sc55::ModulationBlock block,source;
            block.depth = {random(),random(),random()}; block.rateIndex = uint8_t(random());
            source.delayRate = random(); source.attackRate = random(); source.waveform = uint8_t(i%7);
            source.wave = {random(),random(),random(),random()}; source.rateModifier = int16_t(random());
            source.delay = i%3 == 0 ? random() : 65535;
            source.attack = i%3 == 2 ? 65535 : random();
            const uint8_t parameter = uint8_t(i), control = uint8_t(i/256);
            for (unsigned k = 0; k < 3; ++k) MCU_Write16(cpu,0x9380+2*k,block.depth[k]);
            MCU_Write(cpu,0x938c,block.rateIndex);
            const std::array<uint16_t,9> fields{source.delayRate,source.attackRate,uint16_t(source.waveform*2),
                source.wave.phase,source.delay,source.attack,source.wave.held,source.wave.smoothed,source.wave.output};
            for (unsigned k = 0; k < fields.size(); ++k) MCU_Write16(cpu,0x9590+2*k,fields[k]);
            MCU_Write16(cpu,0x958e,uint16_t(source.rateModifier)); MCU_Write(cpu,0x958d,uint8_t(i));
            sc55::FirstModulationSharing sharing{24,17,0};
            const sc55::FirstModulationSharing sourceSharing{uint8_t(i%25),uint8_t(i*17),uint8_t(i)};
            MCU_Write16(cpu,0x93fe,0); MCU_Write16(cpu,0x95fe,1);
            MCU_Write16(cpu,0xc84e,sharing.source); MCU_Write16(cpu,0xc850,sourceSharing.source);
            MCU_Write(cpu,0x93e7,sharing.baseRate); MCU_Write(cpu,0x95e7,sourceSharing.baseRate);
            MCU_Write16(cpu,0x942e,0x9700); MCU_Write(cpu,0x9711,control); MCU_Write16(cpu,0x94a8,parameter);
            cpu.r[0] = 0x9400; cpu.r[2] = 0x9600; cpu.r[7] = 0x9000;
            MCU_Write16(cpu,0x9000,0x7000); execute(0x3d1a,0x7000);
            const auto rate = block.rateIndex;
            if (!sc55::InitializeSharedFirstModulation(block,sharing,source,sourceSharing,parameter,control,depths))
                throw std::runtime_error("Shared first modulation rejected input");
            if (sharing.source != word(0xc84e) || sharing.baseRate != MCU_Read(cpu,0x93e7)
                || sharing.sharing != MCU_Read(cpu,0x938d))
                throw std::runtime_error("Shared first modulation metadata differs");
            const std::array<uint16_t,9> actual{block.delayRate,block.attackRate,uint16_t(block.waveform*2),
                block.wave.phase,block.delay,block.attack,block.wave.held,block.wave.smoothed,block.wave.output};
            for (unsigned k = 0; k < actual.size(); ++k)
                if (actual[k] != word(0x9390+2*k)) throw std::runtime_error("Shared first modulation state differs");
            for (unsigned k = 0; k < 3; ++k)
                if (block.depth[k] != word(0x9380+2*k) || block.output[k] != word(0x9386+2*k))
                    throw std::runtime_error("Shared first modulation depth differs at " + std::to_string(i));
            if (block.rateModifier != int16_t(word(0x938e)) || block.rateIndex != rate || MCU_Read(cpu,0x938c) != rate)
                throw std::runtime_error("Shared first modulation rate differs");
        }
        cpu.r[7] = stack;
        std::printf("Native shared first modulation initialization: 32768 H8 cases matched\n");
    }
    {
        sc55::ModulationDepthTables tables;
        for (unsigned i = 0; i < 128; ++i)
        {
            tables.secondEnvelope[i] = uint16_t((cpu.rom1[0x7212+2*i]<<8)|cpu.rom1[0x7213+2*i]);
            tables.pitch[i] = uint16_t((cpu.rom1[0x7312+2*i]<<8)|cpu.rom1[0x7313+2*i]);
        }
        for (unsigned i = 0; i < 65536; ++i)
        {
            SC55Partial partial{};
            const std::array<unsigned,6> offsets{0x48,0x49,0x2a,0x2b,0x0f,0x0e};
            for (unsigned k = 0; k < offsets.size(); ++k)
            {
                partial.raw[offsets[k]] = uint8_t(k%2 ? i/256+k*19 : i+k*31);
                MCU_Write(cpu,0x9600+offsets[k],partial.raw[offsets[k]]);
            }
            sc55::ModulationBlock first,second;
            first.depth[2] = 1234; first.wave.phase = 3456; second.delay = 5678;
            MCU_Write16(cpu,0x9384,1234); MCU_Write16(cpu,0x9396,3456); MCU_Write16(cpu,0x93ba,5678);
            MCU_Write16(cpu,0x949e,0x9600); cpu.r[0] = 0x9400; execute(0x3800,0x3890);
            const auto rawPitch = sc55::PrepareModulationDepths(partial,first,second,tables);
            const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
            if (rawPitch != word(0x94a8) || first.wave.phase != word(0x9396) || second.delay != word(0x93ba))
                throw std::runtime_error("Modulation depth preparation preservation differs");
            for (unsigned k = 0; k < 3; ++k)
                if (first.depth[k] != word(0x9380+2*k) || second.depth[k] != word(0x93a2+2*k))
                    throw std::runtime_error("Modulation depth preparation differs");
        }
        std::printf("Native modulation depth preparation: 65536 H8 cases matched\n");
    }
    {
        std::array<uint16_t,128> depths{};
        for (unsigned i = 0; i < depths.size(); ++i)
            depths[i] = uint16_t((cpu.rom1[0x7312+2*i]<<8)|cpu.rom1[0x7313+2*i]);
        unsigned checks = 0;
        for (unsigned parameter = 0; parameter < 256; ++parameter)
            for (unsigned control = 0; control < 128; ++control)
            {
                sc55::ModulationBlock block;
                block.depth = {11,22,33}; block.wave.phase = 1234; block.delay = 4321;
                const auto baseRate = uint8_t(parameter), rateControl = uint8_t((control+parameter)%128);
                MCU_Write16(cpu,0x942e,0x9600); MCU_Write(cpu,0x9610,rateControl); MCU_Write(cpu,0x9611,uint8_t(control));
                MCU_Write(cpu,0x93e7,baseRate); MCU_Write16(cpu,0x94a8,uint16_t(parameter));
                cpu.r[0] = 0x9400; execute(0x39f6,0x3a71);
                if (!sc55::PrepareFirstModulationControls(block,baseRate,uint8_t(parameter),rateControl,uint8_t(control),depths)
                    || block.rateIndex != MCU_Read(cpu,0x938c)
                    || block.depth[2] != uint16_t((MCU_Read(cpu,0x9384)<<8)|MCU_Read(cpu,0x9385))
                    || block.depth[0] != 11 || block.depth[1] != 22 || block.wave.phase != 1234 || block.delay != 4321)
                    throw std::runtime_error("First modulation controller mapping differs: " + std::to_string(parameter)+"/"+std::to_string(control));
                ++checks;
            }
        std::printf("Native first modulation controls: %u H8 cases matched\n",checks);
    }
    {
        std::array<uint16_t,256> timing{};
        for (unsigned i = 0; i < timing.size(); ++i)
            timing[i] = uint16_t((cpu.rom1[0x7112+2*i]<<8)|cpu.rom1[0x7113+2*i]);
        unsigned checks = 0;
        for (unsigned mode = 0; mode < 256; ++mode)
            for (unsigned delay = 0; delay < 256; ++delay)
            {
                const auto attack = uint8_t(mode+delay), controller = uint8_t((mode*3+delay)%128);
                sc55::ModulationBlock block;
                block.depth = {11,22,33}; block.wave.held = 0x9876;
                block.rateIndex = 77; block.rateModifier = -123;
                MCU_Write16(cpu,0x942e,0x9700); MCU_Write(cpu,0x9717,controller);
                MCU_Write(cpu,0x9610,uint8_t(delay)); MCU_Write(cpu,0x9611,attack);
                for (unsigned i = 0; i < 3; ++i) MCU_Write16(cpu,0x9380+2*i,block.depth[i]);
                MCU_Write16(cpu,0x939c,block.wave.held); MCU_Write(cpu,0x938c,77); MCU_Write16(cpu,0x938e, uint16_t(-123));
                cpu.r[0] = 0x9400; cpu.r[5] = 0x9600; cpu.r[6] = uint16_t(mode);
                execute(0x38e2,0x3950);
                if (!sc55::PrepareFirstModulation(block,uint8_t(mode),uint8_t(delay),attack,controller,timing))
                    throw std::runtime_error("First modulation setup rejected inputs");
                const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                if (block.waveform*2 != word(0x9394) || block.wave.phase != word(0x9396)
                    || block.wave.smoothed != word(0x939e) || block.wave.output != word(0x93a0)
                    || block.wave.held != word(0x939c) || block.delay != word(0x9398) || block.attack != word(0x939a)
                    || block.delayRate != word(0x9390) || block.attackRate != word(0x9392)
                    || block.rateIndex != MCU_Read(cpu,0x938c) || block.rateModifier != int16_t(word(0x938e)))
                    throw std::runtime_error("First modulation setup differs");
                for (unsigned i = 0; i < 3; ++i)
                    if (block.depth[i] != word(0x9380+2*i) || block.output[i] != word(0x9386+2*i))
                        throw std::runtime_error("First modulation setup depth differs");
                ++checks;
            }
        std::printf("Native first modulation preparation: %u H8 cases matched\n",checks);
    }
    {
        const sc55::LfoWaveformTables tables;
        std::array<uint16_t,256> rates{};
        for (unsigned i = 0; i < rates.size(); ++i)
            rates[i] = uint16_t((cpu.rom1[0x7012+2*i]<<8)|cpu.rom1[0x7013+2*i]);
        const auto stack = cpu.r[7];
        unsigned checks = 0;
        for (unsigned channel = 0; channel < 24; ++channel)
            for (unsigned source = 0; source < 24; ++source)
                for (unsigned reason = 0; reason < 6; ++reason)
                {
                    std::array<sc55::VoiceModulation,24> voices{};
                    std::array<uint8_t,24> sources{};
                    sources.fill(uint8_t(source));
                    auto& destination = voices[channel]; auto& origin = voices[source];
                    origin.firstStage = reason == 1 ? 14 : 12;
                    origin.field99 = 1; origin.field9b = 2; origin.partialIdentity = 0x1234;
                    destination = origin;
                    destination.sharing = reason == 5 ? 0 : 1;
                    if (reason == 2) ++destination.field99;
                    if (reason == 3) ++destination.field9b;
                    if (reason == 4) ++destination.partialIdentity;
                    destination.block.depth = {11,22,33}; destination.block.delayRate = 44;
                    destination.block.attackRate = 55; destination.block.rateIndex = 66;
                    destination.block.waveform = 3;
                    origin.block.output = {101,202,303}; origin.block.rateModifier = -400;
                    origin.block.wave = {123,456,789,987}; origin.block.delay = 654; origin.block.attack = 321;
                    const auto address = [&](unsigned index) { return index == channel ? 0x9400u : index == source ? 0x9600u : 0x9800u+index*2; };
                    const auto map = [&](unsigned index) {
                        const auto a = address(index); const auto& v = voices[index];
                        MCU_Write16(cpu,a,v.firstStage); MCU_Write(cpu,a+0x99,v.field99); MCU_Write(cpu,a+0x9b,v.field9b);
                        MCU_Write16(cpu,a+0x9e,v.partialIdentity); MCU_Write(cpu,a-0x51,v.sharing);
                        const auto b = a-0x5e;
                        for (unsigned i = 0; i < 3; ++i) { MCU_Write16(cpu,b+2*i,v.block.depth[i]); MCU_Write16(cpu,b+6+2*i,v.block.output[i]); }
                        MCU_Write(cpu,b+12,v.block.rateIndex); MCU_Write16(cpu,b+14,uint16_t(v.block.rateModifier));
                        MCU_Write16(cpu,b+16,v.block.delayRate); MCU_Write16(cpu,b+18,v.block.attackRate);
                        MCU_Write16(cpu,b+20,v.block.waveform*2); MCU_Write16(cpu,b+22,v.block.wave.phase);
                        MCU_Write16(cpu,b+24,v.block.delay); MCU_Write16(cpu,b+26,v.block.attack);
                        MCU_Write16(cpu,b+28,v.block.wave.held); MCU_Write16(cpu,b+30,v.block.wave.smoothed); MCU_Write16(cpu,b+32,v.block.wave.output);
                    };
                    map(channel); if (channel != source) map(source);
                    MCU_Write16(cpu,0x93fe,uint16_t(channel));
                    for (unsigned i = 0; i < 24; ++i) MCU_Write16(cpu,0xc87e + 2*i,uint16_t(address(sources[i])));
                    cpu.r[0] = 0x9400; execute(0x3a7a,0x3aea,0x3b26);
                    sc55::VoiceModulationUpdate task;
                    using Update = sc55::VoiceModulationUpdate::Result;
                    const auto route = task.begin(channel,voices,sources);
                    if ((route == Update::shared) != (cpu.pc == 0x3aea))
                        throw std::runtime_error("Modulation route differs");
                    const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                    const auto& b = destination.block;
                    if (destination.sharing != MCU_Read(cpu,0x93af) || b.rateModifier != int16_t(word(0x93b0))
                        || b.wave.phase != word(0x93b8) || b.delay != word(0x93ba) || b.attack != word(0x93bc)
                        || b.wave.held != word(0x93be) || b.wave.smoothed != word(0x93c0) || b.wave.output != word(0x93c2)
                        || b.delayRate != word(0x93b2) || b.attackRate != word(0x93b4) || b.rateIndex != MCU_Read(cpu,0x93ae))
                        throw std::runtime_error("Modulation sharing state differs");
                    for (unsigned i = 0; i < 3; ++i)
                        if (b.depth[i] != word(0x93a2+2*i) || b.output[i] != word(0x93a8+2*i))
                            throw std::runtime_error("Modulation sharing depth differs");
                    for (unsigned i = 0; i < 24; ++i)
                        if (word(0xc87e + 2*i) != (sources[i] == 24 ? 0 : address(sources[i])))
                            throw std::runtime_error("Modulation sharing links differ");
                    if (route == Update::ready)
                    {
                        // Revisit the interrupt window with a simulated stage
                        // mutation. The unshared path has no such window.
                        const bool detach = reason != 5;
                        const bool changed = detach && ((channel+source)&1);
                        if (detach)
                        {
                            cpu.r[0] = 0x9400; execute(0x3b11,0x3b20);
                            if (changed) { ++destination.firstStage; MCU_Write16(cpu,0x9400,destination.firstStage); }
                            execute(0x3b20,0x3b25,0x3b26);
                        }
                        if (!changed)
                        {
                            MCU_Write16(cpu,0xac5a,3); cpu.r[0] = 0x9400; cpu.r[7] = 0x9000;
                            MCU_Write16(cpu,0x9000,0x7000); execute(0x3b26,0x7000);
                        }
                        const auto result = task.resume(voices,3,rates,tables,
                            [](uint8_t)->uint8_t { throw std::runtime_error("Unexpected random waveform read"); },
                            [](uint8_t,uint8_t) { throw std::runtime_error("Unexpected random waveform write"); });
                        if (result != (changed ? Update::stageChanged : Update::updated)
                            || b.wave.phase != word(0x93b8) || b.wave.output != word(0x93c2)
                            || b.delay != word(0x93ba) || b.attack != word(0x93bc))
                            throw std::runtime_error("Modulation update continuation differs");
                        for (unsigned i = 0; i < 3; ++i)
                            if (b.output[i] != word(0x93a8+2*i)) throw std::runtime_error("Modulation continuation depth differs");
                    }
                    ++checks;
                }
        cpu.r[7] = stack;
        std::printf("Native modulation sharing: %u H8 cases matched\n",checks);
    }
    const sc55::PitchConversion pitchConversion;
    unsigned pitchModulationChecks = 0;
    const uint16_t savedStack = cpu.r[7];
    uint32_t modulationSeed = 0x12345678;
    const auto randomWord = [&]() {
        modulationSeed = modulationSeed*1664525u+1013904223u;
        return uint16_t(modulationSeed>>16);
    };
    {
        const sc55::LfoWaveformTables tables;
        // Index 129 is multiplied by zero at the sole endpoint lookup.
        for (unsigned i = 0; i <= 128; ++i)
            if (tables.sine[i] != cpu.rom1[0x7412+i])
                throw std::runtime_error("Generated LFO sine table differs");
        auto hardware = std::make_unique<pcm_t>();
        auto native = std::make_unique<pcm_t>();
        struct Restore { mcu_t& cpu; pcm_t* old; ~Restore() { cpu.pcm = old; } } restore{cpu,cpu.pcm};
        cpu.pcm = hardware.get();
        PCM_Init(*hardware,cpu); PCM_Init(*native,cpu);
        const std::array<uint16_t,7> entries{0x3bee,0x3c31,0x3c48,0x3c5c,0x3cac,0x3cd0,0x3cd3};
        unsigned checks = 0;
        for (unsigned waveform = 0; waveform < 7; ++waveform)
            for (unsigned i = 0; i < 4096; ++i)
            {
                sc55::LfoWaveformState state{uint16_t(i*17),randomWord(),randomWord(),randomWord()};
                const uint32_t increment = i < 16 ? i : (uint32_t(randomWord())<<16)|randomWord();
                hardware->ram2[30][10] = native->ram2[30][10] = randomWord();
                MCU_Write16(cpu,0x9416,state.phase); MCU_Write16(cpu,0x941c,state.held);
                MCU_Write16(cpu,0x941e,state.smoothed); MCU_Write16(cpu,0x9420,state.output);
                cpu.r[1] = 0x9400; cpu.r[4] = uint16_t(increment>>16); cpu.r[5] = uint16_t(increment);
                cpu.r[7] = 0x9000; cpu.br = 0xe0; MCU_Write16(cpu,0x9000,0x7000);
                execute(entries[waveform],0x7000);
                state.advance(uint8_t(waveform),increment,tables,
                    [&](uint8_t a) { return PCM_Read(*native,a); },
                    [&](uint8_t a,uint8_t v) { PCM_Write(*native,a,v); });
                const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                if (state.phase != word(0x9416) || state.held != word(0x941c)
                    || state.smoothed != word(0x941e) || state.output != word(0x9420)
                    || hardware->select_channel != native->select_channel || hardware->read_latch != native->read_latch)
                    throw std::runtime_error("Native LFO waveform differs: " + std::to_string(waveform) + "/" + std::to_string(i));
                ++checks;
            }
        std::printf("Native LFO waveforms: %u H8/PCM cases matched\n",checks);
        std::array<uint16_t,256> rates{};
        for (unsigned i = 0; i < rates.size(); ++i)
            rates[i] = uint16_t((cpu.rom1[0x7012+2*i]<<8)|cpu.rom1[0x7013+2*i]);
        unsigned blockChecks = 0;
        for (unsigned variant = 0; variant < 1024; ++variant)
        {
            sc55::ModulationBlock block;
            block.depth = {randomWord(),randomWord(),randomWord()};
            block.output = {11,22,33};
            block.delay = variant%4 == 0 ? 65535 : uint16_t(variant*61);
            block.attack = variant%4 == 1 ? 65535 : uint16_t(variant*59);
            block.delayRate = uint16_t(variant%3 == 0 ? 0 : variant*13);
            block.attackRate = uint16_t(variant%3 == 1 ? 0 : variant*19);
            block.rateIndex = uint8_t(variant); block.rateModifier = int16_t(randomWord());
            block.waveform = uint8_t(variant%7); block.wave = {randomWord(),randomWord(),randomWord(),randomWord()};
            for (unsigned i = 0; i < 3; ++i)
            {
                MCU_Write16(cpu,0x9400+2*i,block.depth[i]); MCU_Write16(cpu,0x9406+2*i,block.output[i]);
            }
            MCU_Write(cpu,0x940c,block.rateIndex); MCU_Write16(cpu,0x940e,uint16_t(block.rateModifier));
            MCU_Write16(cpu,0x9410,block.delayRate); MCU_Write16(cpu,0x9412,block.attackRate);
            MCU_Write16(cpu,0x9414,uint16_t(block.waveform*2)); MCU_Write16(cpu,0x9416,block.wave.phase);
            MCU_Write16(cpu,0x9418,block.delay); MCU_Write16(cpu,0x941a,block.attack);
            MCU_Write16(cpu,0x941c,block.wave.held); MCU_Write16(cpu,0x941e,block.wave.smoothed);
            MCU_Write16(cpu,0x9420,block.wave.output);
            for (unsigned step = 0; step < 32; ++step)
            {
                const uint16_t ticks = step == 31 ? 65535 : uint16_t(step%5);
                hardware->ram2[30][10] = native->ram2[30][10] = randomWord();
                MCU_Write16(cpu,0xac5a,ticks); cpu.r[1] = 0x9400; cpu.r[7] = 0x9000;
                MCU_Write16(cpu,0x9000,0x7000); cpu.br = 0xe0;
                execute(0x3b2c,0x7000);
                block.advance(ticks,rates,tables,[&](uint8_t a) { return PCM_Read(*native,a); },
                    [&](uint8_t a,uint8_t v) { PCM_Write(*native,a,v); });
                const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                if (block.delay != word(0x9418) || block.attack != word(0x941a)
                    || block.wave.phase != word(0x9416) || block.wave.held != word(0x941c)
                    || block.wave.smoothed != word(0x941e) || block.wave.output != word(0x9420)
                    || hardware->select_channel != native->select_channel || hardware->read_latch != native->read_latch)
                    throw std::runtime_error("Native modulation block differs: " + std::to_string(variant) + "/" + std::to_string(step));
                for (unsigned i = 0; i < 3; ++i)
                    if (block.output[i] != word(0x9406+2*i)) throw std::runtime_error("Native modulation depth differs");
                ++blockChecks;
            }
        }
        cpu.br = 0; cpu.r[7] = savedStack;
        std::printf("Native modulation block: %u persistent H8/PCM updates matched\n",blockChecks);
        std::array<uint16_t,256> timing{};
        std::array<uint16_t,128> depths{};
        for (unsigned i = 0; i < timing.size(); ++i) timing[i] = uint16_t((cpu.rom1[0x7112+2*i]<<8)|cpu.rom1[0x7113+2*i]);
        for (unsigned i = 0; i < depths.size(); ++i) depths[i] = uint16_t((cpu.rom1[0x7312+2*i]<<8)|cpu.rom1[0x7313+2*i]);
        for (unsigned i = 0; i < 4096; ++i)
        {
            sc55::FirstModulationInputs input{uint8_t(i),uint8_t(i*13),uint8_t(i*19),uint8_t(i*31),uint8_t(i*41),
                uint8_t(i%128),uint8_t((i/16)%128),uint8_t((i*3)%128)};
            sc55::ModulationBlock block;
            block.depth = {randomWord(),randomWord(),randomWord()}; block.rateModifier = int16_t(randomWord());
            for (unsigned k = 0; k < 3; ++k) MCU_Write16(cpu,0x9380+2*k,block.depth[k]);
            MCU_Write16(cpu,0x938e,uint16_t(block.rateModifier)); MCU_Write(cpu,0x938d,0);
            MCU_Write16(cpu,0x942e,0x9700); MCU_Write16(cpu,0x94a8,input.pitchDepth);
            MCU_Write(cpu,0x960f,input.baseRate); MCU_Write(cpu,0x9610,input.delay); MCU_Write(cpu,0x9611,input.attack);
            MCU_Write(cpu,0x9710,input.rateControl); MCU_Write(cpu,0x9711,input.depthControl); MCU_Write(cpu,0x9717,input.delayControl);
            hardware->ram2[30][10] = native->ram2[30][10] = randomWord();
            MCU_Write16(cpu,0xac5a,12345); cpu.r[0] = 0x9400; cpu.r[5] = 0x9600; cpu.r[6] = input.mode;
            cpu.r[7] = 0x9000; cpu.br = 0xe0; execute(0x38e2,0x3984);
            if (!sc55::InitializeFirstModulation(block,input,timing,depths,rates,tables,
                [&](uint8_t a) { return PCM_Read(*native,a); },[&](uint8_t a,uint8_t v) { PCM_Write(*native,a,v); }))
                throw std::runtime_error("First modulation initialization rejected inputs");
            const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
            if (block.wave.phase != word(0x9396) || block.wave.held != word(0x939c) || block.wave.smoothed != word(0x939e)
                || block.wave.output != word(0x93a0) || block.delay != word(0x9398) || block.attack != word(0x939a)
                || block.rateIndex != MCU_Read(cpu,0x938c) || block.delayRate != word(0x9390) || block.attackRate != word(0x9392)
                || word(0xac5a) != 12345 || cpu.r[7] != 0x9000
                || hardware->read_latch != native->read_latch || hardware->select_channel != native->select_channel)
                throw std::runtime_error("First modulation initialization differs at " + std::to_string(i));
            for (unsigned k = 0; k < 3; ++k)
                if (block.depth[k] != word(0x9380+2*k) || block.output[k] != word(0x9386+2*k))
                    throw std::runtime_error("First modulation initialization depth differs");
        }
        cpu.br = 0; cpu.r[7] = savedStack;
        std::printf("Native first modulation initialization: 4096 H8/PCM cases matched\n");
        for (unsigned v = 0; v < 24; ++v)
        {
            const auto at = unsigned(0x676a+2*v);
            MCU_Write16(cpu,unsigned((MCU_Read(cpu,at)<<8)|MCU_Read(cpu,at+1)),14);
        }
        for (unsigned i = 0; i < 4096; ++i)
        {
            SC55Partial partial{};
            partial.raw[4] = uint8_t(i); partial.raw[5] = uint8_t(i*13);
            partial.raw[6] = uint8_t(i*19); partial.raw[7] = uint8_t(i*31);
            // Keep exhaustive byte coverage, then break correlations between
            // mode, rate, delay and attack for mixed initialization states.
            if (i >= 256)
                for (unsigned k = 4; k < 8; ++k) partial.raw[k] = uint8_t(randomWord());
            std::array<sc55::VoiceModulation,24> voices{};
            for (auto& voice : voices) voice.firstStage = 14;
            auto& block = voices[0].block;
            std::array<uint8_t,24> sources{}; sources.fill(24);
            sc55::VoiceStopState stop; stop.flagMinus3B = uint8_t(128|(i&127));
            partial.raw[8] = uint8_t(i*17);
            MCU_Write(cpu,0x93c5,stop.flagMinus3B); MCU_Write(cpu,0x9499,0);
            MCU_Write16(cpu,0x949e,0x9600); MCU_Write16(cpu,0x93fe,0);
            MCU_Write(cpu,0x9608,partial.raw[8]);
            block.depth = {randomWord(),randomWord(),randomWord()}; block.rateModifier = int16_t(randomWord());
            for (unsigned k = 0; k < 3; ++k) MCU_Write16(cpu,0x93a2+2*k,block.depth[k]);
            MCU_Write16(cpu,0x93b0,uint16_t(block.rateModifier)); MCU_Write(cpu,0x93af,0);
            for (unsigned k = 4; k < 8; ++k) MCU_Write(cpu,0x9600+k,partial.raw[k]);
            hardware->ram2[30][10] = native->ram2[30][10] = randomWord();
            MCU_Write16(cpu,0xac5a,23456); cpu.r[0] = 0x9400; cpu.r[5] = 0x9600; cpu.r[6] = partial.raw[4];
            cpu.r[7] = 0x9000; cpu.br = 0xe0; execute(0x2a66,0x2bac);
            const auto route = sc55::PrepareVoiceSecondModulation(0,stop,voices,sources,partial,timing,rates,tables,
                [&](uint8_t a) { return PCM_Read(*native,a); },[&](uint8_t a,uint8_t v) { PCM_Write(*native,a,v); });
            if (route != sc55::SecondModulationPreparation::local || sources[0] != 24
                || MCU_Read(cpu,0xc87e) != 0 || MCU_Read(cpu,0xc87f) != 0
                || voices[0].fieldA2 != MCU_Read(cpu,0x94a2) || stop.flagMinus3B != MCU_Read(cpu,0x93c5))
                throw std::runtime_error("Second modulation gated local metadata differs");
            const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
            if (block.wave.phase != word(0x93b8) || block.wave.held != word(0x93be) || block.wave.smoothed != word(0x93c0)
                || block.wave.output != word(0x93c2) || block.delay != word(0x93ba) || block.attack != word(0x93bc)
                || block.waveform*2 != word(0x93b6) || block.rateModifier != int16_t(word(0x93b0))
                || block.rateIndex != MCU_Read(cpu,0x93ae) || block.delayRate != word(0x93b2) || block.attackRate != word(0x93b4)
                || word(0xac5a) != 23456 || cpu.r[7] != 0x9000
                || hardware->read_latch != native->read_latch || hardware->select_channel != native->select_channel)
                throw std::runtime_error("Second modulation initialization differs at " + std::to_string(i));
            for (unsigned k = 0; k < 3; ++k)
                if (block.depth[k] != word(0x93a2+2*k) || block.output[k] != word(0x93a8+2*k))
                    throw std::runtime_error("Second modulation initialization depth differs");
        }
        cpu.br = 0; cpu.r[7] = savedStack;
        std::printf("Native second modulation initialization: 4096 H8/PCM cases matched\n");
    }
    for (unsigned i = 0; i < 16384; ++i)
    {
        const std::array<uint16_t,8> edges{0,1,6000,32767,32768,32769,65534,65535};
        const std::array<uint32_t,8> pitches{0,1,32768,65535,127000,0x7fffff,0x800000,0xffffff};
        sc55::PitchModulationInputs input;
        const uint32_t initial = pitches[i%8];
        input.offset = i%2 ? randomWord() : edges[(i/8)%8];
        for (unsigned s = 0; s < 2; ++s)
        {
            input.sources[s] = {edges[(i/64+s)%8],edges[(i/512+s)%8],edges[(i/8+s)%8]};
            if (i >= 8192) input.sources[s] = {randomWord(),randomWord(),randomWord()};
        }
        input.masterTune = randomWord(); input.partTune = randomWord();
        cpu.r[7] = 0x9000; cpu.r[0] = 0x9400;
        MCU_Write(cpu,0x942d,uint8_t(initial>>16)); MCU_Write16(cpu,0x9446,uint16_t(initial));
        MCU_Write(cpu,0x942c,0x12); MCU_Write16(cpu,0x9444,0x3456);
        MCU_Write16(cpu,0x9486,input.offset);
        MCU_Write16(cpu,0x938a,input.sources[0].first); MCU_Write16(cpu,0x9490,input.sources[0].second); MCU_Write16(cpu,0x93a0,input.sources[0].waveform);
        MCU_Write16(cpu,0x93ac,input.sources[1].first); MCU_Write16(cpu,0x9492,input.sources[1].second); MCU_Write16(cpu,0x93c2,input.sources[1].waveform);
        MCU_Write16(cpu,0x8000,input.masterTune); MCU_Write16(cpu,0x93fe,0); MCU_Write(cpu,0xc8e4,0); MCU_Write16(cpu,0xab76,input.partTune);
        execute(0x50cf,0x5175);
        sc55::ModulationBlock firstBlock,secondBlock;
        firstBlock.output = {123,456,input.sources[0].first}; firstBlock.wave.output = input.sources[0].waveform;
        secondBlock.output = {789,987,input.sources[1].first}; secondBlock.wave.output = input.sources[1].waveform;
        sc55::SecondEnvelopeOutputInputs unusedSecondInput;
        for (auto& source : input.sources) { source.first = 0; source.waveform = 0; }
        sc55::ApplyVoiceModulationOutputs(firstBlock,secondBlock,unusedSecondInput,input);
        const uint32_t actual = (uint32_t(MCU_Read(cpu,0x942d))<<16)|(uint32_t(MCU_Read(cpu,0x9446))<<8)|MCU_Read(cpu,0x9447);
        if (actual != sc55::PrepareModulatedPitch(initial,input)
            || MCU_Read(cpu,0x942c) != 0x12 || MCU_Read(cpu,0x9444) != 0x34 || MCU_Read(cpu,0x9445) != 0x56)
            throw std::runtime_error("Pitch modulation chain differs at " + std::to_string(i));
        ++pitchModulationChecks;
    }
    cpu.r[7] = savedStack;
    std::printf("Native pitch modulation chain: %u H8 cases matched\n",pitchModulationChecks);
    unsigned pitchRunnerChecks = 0;
    unsigned secondOutputChecks = 0;
    for (unsigned mode = 0; mode < 256; ++mode)
    {
        sc55::SecondEnvelopePcmState pcm{17,2345,3456,4567};
        sc55::SecondEnvelopeActivation activation{5678,6789};
        MCU_Write(cpu,0x9627,uint8_t(mode)); MCU_Write(cpu,0x9465,99); MCU_Write(cpu,0x9466,88); MCU_Write(cpu,0x9468,pcm.control);
        MCU_Write16(cpu,0x9422,pcm.output); MCU_Write16(cpu,0x9424,pcm.level); MCU_Write16(cpu,0x9426,pcm.command);
        MCU_Write16(cpu,0x93ea,activation.level); MCU_Write16(cpu,0x93e8,activation.command);
        cpu.r[0] = 0x9400; cpu.r[5] = 0x9600;
        execute(0x3e3b,0x3e69,0x4435);
        const auto result = sc55::ConfigureSecondEnvelopeMode(uint8_t(mode),pcm,activation);
        const auto word = [&](unsigned at) { return uint16_t((MCU_Read(cpu,at)<<8)|MCU_Read(cpu,at+1)); };
        if (result.bypass != (MCU_Read(cpu,0x9465) != 0) || result.pcmMode != MCU_Read(cpu,0x9466)
            || pcm.control != MCU_Read(cpu,0x9468) || pcm.output != word(0x9422)
            || pcm.level != word(0x9424) || pcm.command != word(0x9426)
            || activation.level != word(0x93ea) || activation.command != word(0x93e8))
            throw std::runtime_error("Second envelope mode differs at " + std::to_string(mode));
    }
    std::printf("Native second envelope mode: 256 H8 cases matched\n");
    sc55::SecondEnvelopeTargetTables secondTargetTables;
    const auto loadTargetWords = [&](auto& words,unsigned at) {
        for (unsigned j = 0; j < words.size(); ++j) words[j] = uint16_t((cpu.rom1[at+2*j]<<8)|cpu.rom1[at+2*j+1]);
    };
    loadTargetWords(secondTargetTables.startSensitivity,0x74d2);
    loadTargetWords(secondTargetTables.velocitySensitivity,0x74fc);
    loadTargetWords(secondTargetTables.scale,0x7512);
    sc55::SecondEnvelopeKeyTables secondKeys;
    for (unsigned selector = 0; selector < 16; ++selector)
    {
        const unsigned at = 0x3dcd2+selector*2;
        const unsigned pointer = (cpu.rom2[at]<<8)|cpu.rom2[at+1];
        for (unsigned key = 0; key < 256; ++key)
            secondKeys[selector][key] = uint16_t((cpu.rom2[0x30000+((pointer+key*2)&65535)]<<8)
                |cpu.rom2[0x30000+((pointer+key*2+1)&65535)]);
    }
    for (unsigned i = 0; i < 65536; ++i)
    {
        const auto selector = uint8_t(i/256), key = uint8_t(i);
        MCU_Write(cpu,0x9628,selector); MCU_Write(cpu,0xc8fc,key);
        cpu.r[0] = 0x9400; cpu.r[1] = 0; cpu.r[5] = 0x9600;
        execute(0x3e79,0x3e98);
        if (cpu.r[2] != secondKeys[selector&15][key]) throw std::runtime_error("Second envelope key lookup differs");
    }
    std::printf("Native second envelope key lookup: 65536 H8 cases matched\n");
    sc55::SecondEnvelopeTimingKeyCurves secondTimingCurves;
    std::array<uint16_t,256> secondTimingMultipliers;
    loadTargetWords(secondTimingMultipliers,0x67c6);
    for (unsigned selector = 0; selector < 16; ++selector)
        for (unsigned which = 0; which < 2; ++which)
        {
            const unsigned at = (which ? 0x3dd12 : 0x3dcf2)+selector*2;
            const unsigned pointer = (cpu.rom2[at]<<8)|cpu.rom2[at+1];
            auto& row = which ? secondTimingCurves.release[selector] : secondTimingCurves.attack[selector];
            for (unsigned key = 0; key < 256; ++key) row[key] = cpu.rom2[0x30000+((pointer+key)&65535)];
        }
    for (unsigned i = 0; i < 16*256*41; ++i)
    {
        SC55Partial partial;
        partial.raw[0x39] = uint8_t(i%16); partial.raw[0x3a] = uint8_t(15-i%16);
        partial.raw[0x3b] = uint8_t(44+i/4096); partial.raw[0x3c] = uint8_t(84-i/4096);
        partial.raw[0x3e] = uint8_t(44+(i*17)%41); partial.raw[0x3f] = uint8_t(44+(i*31)%41);
        const auto key = uint8_t(i/16), amplitude = uint8_t(i*13);
        for (unsigned j = 0x39; j <= 0x3f; ++j) MCU_Write(cpu,0x9600+j,partial.raw[j]);
        MCU_Write(cpu,0xc8fc,key); MCU_Write(cpu,0xc95c,amplitude);
        cpu.r[0] = 0x9400; cpu.r[1] = 0; cpu.r[5] = 0x9600;
        execute(0x425b,0x43d1);
        const auto native = sc55::PrepareSecondEnvelopeTiming(partial,key,amplitude,secondTimingCurves,secondTimingMultipliers);
        const auto word = [&](unsigned at) { return uint16_t((MCU_Read(cpu,at)<<8)|MCU_Read(cpu,at+1)); };
        if (!native || native->keyScale != word(0x93d4) || native->releaseKeyScale != word(0x93d6)
            || native->attackVelocityScale != word(0x93d8) || native->decayReleaseVelocityScale != word(0x93da))
            throw std::runtime_error("Second envelope timing preparation differs at " + std::to_string(i));
    }
    std::printf("Native second envelope timing preparation: 167936 H8 cases matched\n");
    for (unsigned i = 0; i < 65536; ++i)
    {
        SC55Partial partial;
        partial.raw[0x29] = uint8_t(i); partial.raw[0x3d] = uint8_t(i/256); partial.raw[0x2c] = uint8_t(i*31);
        const auto keyValue = uint16_t(i*977); const auto amplitude = uint8_t(i*17);
        MCU_Write(cpu,0x9629,partial.raw[0x29]); MCU_Write(cpu,0x963d,partial.raw[0x3d]); MCU_Write(cpu,0x962c,partial.raw[0x2c]);
        MCU_Write(cpu,0xc95c,amplitude);
        cpu.r[0] = 0x9400; cpu.r[1] = 0; cpu.r[2] = keyValue; cpu.r[5] = 0x9600;
        execute(0x3e98,0x3f16);
        const auto native = sc55::PrepareSecondEnvelopeTargetInputs(partial,keyValue,amplitude,secondTargetTables);
        const auto word = [&](unsigned at) { return uint16_t((MCU_Read(cpu,at)<<8)|MCU_Read(cpu,at+1)); };
        if (native.start != word(0x9454) || native.depth != word(0x93d2) || native.scale != word(0x93dc))
            throw std::runtime_error("Second envelope target inputs differ at " + std::to_string(i));
    }
    std::printf("Native second envelope target inputs: 65536 H8 cases matched\n");
    std::array<uint8_t,256> secondTargetCurve;
    std::copy_n(cpu.rom1+0x79f2,256,secondTargetCurve.begin());
    for (unsigned i = 0; i < 65536; ++i)
    {
        const auto start = uint16_t(i*977), depth = uint16_t(i*313), scale = uint16_t(i*47);
        const auto parameter = uint8_t(i);
        MCU_Write16(cpu,0x9454,start); MCU_Write16(cpu,0x93d2,depth); MCU_Write16(cpu,0x93dc,scale);
        MCU_Write(cpu,0x9631,parameter); cpu.r[0] = 0x9400; cpu.r[5] = 0x9600;
        execute(0x410d,0x418c);
        const auto target = sc55::PrepareSecondEnvelopeTarget(start,depth,scale,parameter,secondTargetCurve);
        if (target != uint16_t((MCU_Read(cpu,0x945e)<<8)|MCU_Read(cpu,0x945f)))
            throw std::runtime_error("Second envelope target differs at " + std::to_string(i));
    }
    std::printf("Native second envelope target: 65536 H8 cases matched\n");
    for (unsigned i = 0; i < 4096; ++i)
    {
        const auto start = uint16_t(i*977), depth = uint16_t(i*313), scale = uint16_t(i*47);
        SC55Partial partial;
        for (unsigned j = 0x2d; j <= 0x36; ++j)
        { partial.raw[j] = uint8_t(i*(j*2+1)+j); MCU_Write(cpu,0x9600+j,partial.raw[j]); }
        sc55::SecondEnvelopeReleaseState native;
        native.stage = 12; native.progress = {123,456}; native.level = 789; native.stepTime = 8;
        MCU_Write16(cpu,0x9454,start); MCU_Write16(cpu,0x93d2,depth);
        cpu.r[0] = 0x9400; cpu.r[5] = 0x9600; cpu.r[2] = depth; cpu.r[3] = scale;
        execute(0x3f13,0x418c);
        execute(0x43d1,0x4403);
        native.installTargets(partial,start,depth,scale,secondTargetCurve);
        const auto word = [&](unsigned at) { return uint16_t((MCU_Read(cpu,at)<<8)|MCU_Read(cpu,at+1)); };
        if (native.start != word(0x9454) || native.target != word(0x9456) || native.releaseTarget != word(0x945e)
            || native.parameter != MCU_Read(cpu,0x944a) || native.releaseParameter != MCU_Read(cpu,0x944e)
            || native.stage != 12 || native.progress.position != 123 || native.progress.deferredTicks != 456 || native.level != 789 || native.stepTime != 8)
            throw std::runtime_error("Second envelope target installation differs");
        for (unsigned j = 0; j < 3; ++j)
            if (native.nextTargets[j] != word(0x9458+j*2) || native.nextParameters[j] != MCU_Read(cpu,0x944b+j))
                throw std::runtime_error("Second envelope future target installation differs");
    }
    std::printf("Native second envelope target installation: 4096 H8 cases matched\n");
    for (unsigned i = 0; i < 4096; ++i)
    {
        SC55Partial partial;
        for (unsigned j = 0; j < SC55Partial::SIZE; ++j)
        { partial.raw[j] = uint8_t(i*(j*2+1)+j); MCU_Write(cpu,0x9600+j,partial.raw[j]); }
        const auto key = uint8_t(i), amplitude = uint8_t(i*17);
        MCU_Write(cpu,0xc8fc,key); MCU_Write(cpu,0xc95c,amplitude);
        cpu.r[0] = 0x9400; cpu.r[1] = 0; cpu.r[5] = 0x9600;
        execute(0x3e79,0x418c); execute(0x43d1,0x4403);
        sc55::SecondEnvelopeReleaseState state;
        state.prepareTargets(partial,key,amplitude,secondKeys,secondTargetTables,secondTargetCurve);
        const auto word = [&](unsigned at) { return uint16_t((MCU_Read(cpu,at)<<8)|MCU_Read(cpu,at+1)); };
        if (state.start != word(0x9454) || state.target != word(0x9456) || state.releaseTarget != word(0x945e)
            || state.parameter != MCU_Read(cpu,0x944a) || state.releaseParameter != MCU_Read(cpu,0x944e))
            throw std::runtime_error("Composed second envelope targets differ");
        for (unsigned j = 0; j < 3; ++j)
            if (state.nextTargets[j] != word(0x9458+j*2) || state.nextParameters[j] != MCU_Read(cpu,0x944b+j))
                throw std::runtime_error("Composed second envelope future targets differ");
    }
    std::printf("Native second envelope composed targets: 4096 H8 cases matched\n");
    sc55::SecondEnvelopePcmTables secondPcmTables;
    for (unsigned i = 0; i < 256; ++i)
    {
        secondPcmTables.smoothingCeiling[i] = cpu.rom1[0x7714+i];
    }
    for (unsigned i = 0; i < secondPcmTables.outputCeiling.size(); ++i)
        secondPcmTables.outputCeiling[i] = cpu.rom1[0x7816+i];
    for (unsigned i = 0; i < secondPcmTables.levelCurve.size(); ++i)
        secondPcmTables.levelCurve[i] = uint16_t((cpu.rom1[0x7612+i*2]<<8)|cpu.rom1[0x7613+i*2]);
    unsigned secondPcmChecks = 0;
    sc55::EnvelopeTimes secondSetupTimes;
    loadTargetWords(secondSetupTimes,0x6f12);
    for (unsigned i = 0; i < 4096; ++i)
    {
        SC55Partial partial;
        for (unsigned j = 0; j < SC55Partial::SIZE; ++j) partial.raw[j] = uint8_t((i*(j*2+1)+j)%128);
        partial.raw[0x27] = uint8_t(i%4);
        partial.raw[0x39] %= 16; partial.raw[0x3a] %= 16;
        for (unsigned j : {0x3bu,0x3cu,0x3eu,0x3fu}) partial.raw[j] = uint8_t(44+partial.raw[j]%41);
        for (unsigned j = 0; j < SC55Partial::SIZE; ++j) MCU_Write(cpu,0x9600+j,partial.raw[j]);
        const auto key = uint8_t(i), amplitude = uint8_t(i*13);
        sc55::SecondEnvelopeReleaseState segment;
        segment.stage = uint16_t((i%12)*2); segment.level = 123; segment.progress = {456,789};
        sc55::SecondEnvelopePcmState pcm{17,2345,3456,4567};
        sc55::SecondEnvelopeSetup setup;
        setup.controlBase = 21; setup.limit = 22; setup.input.base = 23;
        setup.input.control = uint8_t(i%128); setup.controller = uint8_t((i*7)%128);
        setup.input.suppressPositiveControl = i%2; setup.timing.attackControlEnabled = (i/2)%2;
        setup.input.offset = uint16_t(i*313);
        MCU_Write(cpu,0xc8fc,key); MCU_Write(cpu,0xc95c,amplitude);
        MCU_Write16(cpu,0x93fe,0); MCU_Write16(cpu,0x942e,0x9700); MCU_Write16(cpu,0x949e,0x9600); MCU_Write(cpu,0x9499,0);
        MCU_Write(cpu,0x9712,setup.input.control); MCU_Write(cpu,0x9713,setup.controller);
        for (unsigned j = 0; j < 3; ++j) MCU_Write(cpu,0x9714+j,64);
        MCU_Write(cpu,0x94a2,uint8_t((setup.input.suppressPositiveControl ? 4 : 0)|(setup.timing.attackControlEnabled ? 16 : 0)));
        MCU_Write(cpu,0x94a3,23); MCU_Write(cpu,0x9467,21); MCU_Write(cpu,0x9469,22); MCU_Write(cpu,0x9468,17);
        for (unsigned at : {0x9388u,0x948cu,0x93a0u,0x93aau,0x9494u,0x93c2u}) MCU_Write16(cpu,at,0);
        MCU_Write16(cpu,0x9488,setup.input.offset);
        MCU_Write16(cpu,0x9402,segment.stage); MCU_Write16(cpu,0x9420,123); MCU_Write16(cpu,0x940a,456); MCU_Write16(cpu,0x9414,789);
        MCU_Write16(cpu,0x9422,pcm.output); MCU_Write16(cpu,0x9424,pcm.level); MCU_Write16(cpu,0x9426,pcm.command);
        cpu.r[0] = 0x9400; cpu.r[5] = 0x9600; cpu.r[7] = 0x9000;
        execute(0x3e3b,0x4435);
        if (!setup.prepare(partial,key,amplitude,segment,pcm,secondKeys,secondTargetTables,secondTargetCurve,
            secondTimingCurves,secondTimingMultipliers,secondSetupTimes,secondPcmTables)) throw std::runtime_error("Second envelope setup rejected inputs");
        const auto word = [&](unsigned at) { return uint16_t((MCU_Read(cpu,at)<<8)|MCU_Read(cpu,at+1)); };
        if (setup.mode.bypass != (MCU_Read(cpu,0x9465)!=0) || setup.mode.pcmMode != MCU_Read(cpu,0x9466)
            || setup.input.base != MCU_Read(cpu,0x94a3) || setup.controlBase != MCU_Read(cpu,0x9467) || setup.limit != MCU_Read(cpu,0x9469)
            || setup.activation.level != word(0x93ea) || setup.activation.command != word(0x93e8)
            || pcm.control != MCU_Read(cpu,0x9468) || pcm.output != word(0x9422) || pcm.level != word(0x9424) || pcm.command != word(0x9426)
            || segment.stage != word(0x9402) || segment.level != word(0x9420) || segment.progress.position != word(0x940a) || segment.progress.deferredTicks != word(0x9414))
            throw std::runtime_error("Full second envelope setup differs at " + std::to_string(i));
    }
    cpu.r[7] = savedStack;
    std::printf("Native full second envelope setup: 4096 H8 cases matched\n");
    for (unsigned i = 0; i < 65536; ++i)
    {
        sc55::SecondEnvelopeReleaseState state;
        std::array<uint16_t,6> targets;
        for (unsigned j = 0; j < targets.size(); ++j)
        { targets[j] = uint16_t(i*(j*211+1)+j*8191); MCU_Write16(cpu,0x9454+j*2,targets[j]); }
        state.start = targets[0]; state.target = targets[1];
        state.nextTargets = {targets[2],targets[3],targets[4]}; state.releaseTarget = targets[5];
        const auto levelBase = uint8_t(i%128), levelControl = uint8_t((i/128)%128);
        const auto base = uint8_t((i*17)%128), controller = uint8_t((i*31)%128);
        const bool suppress = (i/16384)%2;
        MCU_Write16(cpu,0x942e,0x9600); MCU_Write(cpu,0x94a3,levelBase); MCU_Write(cpu,0x9612,levelControl);
        MCU_Write(cpu,0x94a2,suppress ? 4 : 0); MCU_Write(cpu,0x9467,base); MCU_Write(cpu,0x9613,controller);
        cpu.r[0] = 0x9400; execute(0x418c,0x4253);
        const auto native = sc55::PrepareSecondEnvelopeInitialControl(state,levelBase,levelControl,suppress,base,controller,secondPcmTables);
        if (!native || native->limit != MCU_Read(cpu,0x9469) || native->control != MCU_Read(cpu,0x9468))
            throw std::runtime_error("Initial second envelope controller differs at " + std::to_string(i));
    }
    std::printf("Native initial second envelope controller: 65536 H8 cases matched\n");
    unsigned secondCommandChecks = 0;
    for (unsigned i = 0; i < 65536; ++i)
    {
        const auto base = uint8_t(i%128), control = uint8_t((i/128)%128);
        const auto limit = uint8_t((i*17)%128), initial = uint8_t((i*71)%128);
        const auto previous = uint16_t(i*977), level = uint16_t(i%32768);
        MCU_Write16(cpu,0x942e,0x9600); MCU_Write(cpu,0x9613,control);
        MCU_Write(cpu,0x9467,base); MCU_Write(cpu,0x9468,initial); MCU_Write(cpu,0x9469,limit);
        MCU_Write16(cpu,0x9424,previous); MCU_Write16(cpu,0x9422,level);
        cpu.r[0] = 0x9400; execute(0x46e0,0x473c);
        const auto smoothed = sc55::AdvanceSecondEnvelopeControl(initial,base,control,limit,previous,secondPcmTables);
        if (!smoothed || *smoothed != MCU_Read(cpu,0x9468))
            throw std::runtime_error("Second envelope smoothing differs at " + std::to_string(i));
        auto nativeControl = *smoothed;
        const auto nativeLevel = sc55::ConvertSecondEnvelopePcmLevel(level,nativeControl,secondPcmTables);
        execute(0x473c,0x478e);
        const auto actualLevel = uint16_t((MCU_Read(cpu,0x9424)<<8)|MCU_Read(cpu,0x9425));
        if (nativeLevel != actualLevel || nativeControl != MCU_Read(cpu,0x9468))
            throw std::runtime_error("Second envelope PCM level differs at " + std::to_string(i));
        // Complete the original routine including its RTS, with a private
        // return address. Compare every supported step time on the same level.
        for (uint16_t stepTime = 0; stepTime <= 8; ++stepTime)
        {
            const uint16_t oldLevel = i%3 == 0 ? actualLevel
                : i%3 == 1 ? uint16_t(actualLevel == 0 ? 0 : actualLevel-1) : previous;
            MCU_Write16(cpu,0x93d0,stepTime);
            cpu.r[5] = oldLevel; cpu.r[6] = actualLevel;
            cpu.r[7] = 0x9000; MCU_Write16(cpu,0x9000,0x7000);
            execute(0x478e,0x7000);
            const auto command = sc55::EncodeSecondEnvelopePcmCommand(oldLevel,actualLevel,nativeControl,stepTime,secondPcmTables);
            const auto actualCommand = uint16_t((MCU_Read(cpu,0x9426)<<8)|MCU_Read(cpu,0x9427));
            if (!command || *command != actualCommand)
                throw std::runtime_error("Second envelope PCM command differs at " + std::to_string(i) + "/" + std::to_string(stepTime));
            ++secondCommandChecks;
        }
        ++secondPcmChecks;
    }
    cpu.r[7] = savedStack;
    std::printf("Native second envelope PCM level: %u H8 cases matched\n",secondPcmChecks);
    std::printf("Native second envelope PCM command: %u H8 cases matched\n",secondCommandChecks);
    for (unsigned i = 0; i < 16384; ++i)
    {
        sc55::SecondEnvelopeOutputInputs input;
        input.base = uint8_t(i%128); input.control = uint8_t((i/128)%128);
        input.suppressPositiveControl = i%2; input.offset = uint16_t(i*313);
        input.sources[0] = {uint16_t(i*257),uint16_t(i*519),uint16_t(i*127)};
        input.sources[1] = {uint16_t(i*977),uint16_t(i*41),uint16_t(i*1023)};
        const uint16_t initial = uint16_t(i*1237);
        MCU_Write16(cpu,0x93fe,0); MCU_Write16(cpu,0x942e,0x9600);
        MCU_Write(cpu,0x94a3,input.base); MCU_Write(cpu,0x9612,input.control); MCU_Write(cpu,0x94a2,input.suppressPositiveControl ? 4 : 0);
        MCU_Write16(cpu,0x9488,input.offset);
        MCU_Write16(cpu,0x9388,input.sources[0].first); MCU_Write16(cpu,0x948c,input.sources[0].second); MCU_Write16(cpu,0x93a0,input.sources[0].waveform);
        MCU_Write16(cpu,0x93aa,input.sources[1].first); MCU_Write16(cpu,0x9494,input.sources[1].second); MCU_Write16(cpu,0x93c2,input.sources[1].waveform);
        cpu.r[0] = 0x9400; cpu.r[5] = initial; cpu.r[7] = 0x9000; execute(0x4662,0x46e0);
        sc55::ModulationBlock firstBlock,secondBlock;
        firstBlock.output = {123,input.sources[0].first,456}; firstBlock.wave.output = input.sources[0].waveform;
        secondBlock.output = {789,input.sources[1].first,987}; secondBlock.wave.output = input.sources[1].waveform;
        sc55::PitchModulationInputs unusedPitchInput;
        for (auto& source : input.sources) { source.first = 0; source.waveform = 0; }
        sc55::ApplyVoiceModulationOutputs(firstBlock,secondBlock,input,unusedPitchInput);
        const auto expected = sc55::PrepareSecondEnvelopeOutput(initial,input);
        const auto actual = uint16_t((MCU_Read(cpu,0x9422)<<8)|MCU_Read(cpu,0x9423));
        if (!expected || *expected != actual) throw std::runtime_error("Second envelope output modulation differs at " + std::to_string(i));
        sc55::SecondEnvelopePcmState native;
        native.control = uint8_t((i*31)%128); native.level = uint16_t(i*419);
        const auto base = uint8_t(i%128), controller = uint8_t((i/128)%128), limit = uint8_t((i*7)%128);
        const auto stepTime = uint16_t(i%9);
        MCU_Write(cpu,0x9467,base); MCU_Write(cpu,0x9468,native.control); MCU_Write(cpu,0x9469,limit);
        MCU_Write(cpu,0x9613,controller); MCU_Write16(cpu,0x9424,native.level); MCU_Write16(cpu,0x93d0,stepTime);
        cpu.r[5] = initial; cpu.r[7] = 0x9000; MCU_Write16(cpu,0x9000,0x7000);
        execute(0x4662,0x7000);
        if (!native.advance(initial,input,base,controller,limit,stepTime,secondPcmTables)
            || native.output != uint16_t((MCU_Read(cpu,0x9422)<<8)|MCU_Read(cpu,0x9423))
            || native.level != uint16_t((MCU_Read(cpu,0x9424)<<8)|MCU_Read(cpu,0x9425))
            || native.command != uint16_t((MCU_Read(cpu,0x9426)<<8)|MCU_Read(cpu,0x9427))
            || native.control != MCU_Read(cpu,0x9468))
            throw std::runtime_error("Second envelope post-processing chain differs at " + std::to_string(i));
        ++secondOutputChecks;
    }
    cpu.r[7] = savedStack;
    std::printf("Native second envelope output modulation: %u H8 cases matched\n",secondOutputChecks);
    unsigned secondSegmentChecks = 0;
    for (uint16_t start : {uint16_t(0),uint16_t(1),uint16_t(32767),uint16_t(32768),uint16_t(65535)})
        for (uint16_t target : {uint16_t(0),uint16_t(1),uint16_t(32767),uint16_t(32768),uint16_t(65535)})
            for (uint16_t duration : {uint16_t(0),uint16_t(8),uint16_t(9),uint16_t(256),uint16_t(32768),uint16_t(65535)})
            {
                sc55::SecondEnvelopeReleaseState state;
                state.start = start; state.target = target; state.progress = {123,17};
                MCU_Write16(cpu,0x9454,start); MCU_Write16(cpu,0x9456,target);
                MCU_Write16(cpu,0x940a,123); MCU_Write16(cpu,0x9414,17);
                for (unsigned tick = 0; tick < 32; ++tick)
                {
                    const uint16_t elapsed = uint16_t(tick == 31 ? 65535 : tick%4);
                    MCU_Write16(cpu,0xac5a,elapsed); cpu.r[0] = 0x9400; cpu.r[6] = duration;
                    execute(0x45b6,0x4662);
                    state.advanceSegment(duration,elapsed);
                    const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                    if (state.level != word(0x9420) || state.stepTime != word(0x93d0)
                        || state.progress.position != word(0x940a) || state.progress.deferredTicks != word(0x9414))
                        throw std::runtime_error("Second envelope segment differs");
                    ++secondSegmentChecks;
                }
            }
    std::printf("Native second envelope segment: %u persistent H8 updates matched\n",secondSegmentChecks);
    unsigned voiceReleaseChecks = 0;
    for (unsigned stage = 0; stage < 8; ++stage)
        for (unsigned variant = 0; variant < 12; ++variant)
        {
            sc55::EnvelopeRunner::Setup setup{};
            setup.delayIncrement = uint16_t(variant%3); setup.plan.stages[4] = {55,66};
            sc55::EnvelopeRunner amplitude(setup,{{sc55::EnvelopeStage(stage),{123,17},{10,20},30,40},uint16_t(variant*5000),0xff00,1234});
            sc55::PitchEnvelopeRunner pitch;
            pitch.stage = 8; pitch.output = 60000; pitch.segment = {{321,9},12345,65000,32768,0};
            pitch.releaseTarget = 55000; pitch.releaseIncrement = 8192;
            sc55::VoiceReleaseAuxiliary aux;
            aux.pending = uint8_t(variant%4); aux.activity = 99;
            aux.second = {6,{456,7},22,33,4444,5555,6666,7777};
            aux.modulationReleaseWords = {uint16_t(variant%2 ? 0 : 123),uint16_t(variant%3 ? 0 : 456)};
            const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
            const auto put24 = [&](unsigned hi,unsigned lo,uint32_t v) { MCU_Write(cpu,hi,uint8_t(v>>16)); MCU_Write16(cpu,lo,uint16_t(v)); };
            MCU_Write16(cpu,0x93fe,0); MCU_Write(cpu,0xac2a,aux.pending); MCU_Write(cpu,0xac42,99);
            MCU_Write16(cpu,0x9400,uint16_t(stage == 7 ? 22 : stage*2)); MCU_Write16(cpu,0x9402,6); MCU_Write16(cpu,0x9404,8);
            MCU_Write16(cpu,0x9410,setup.delayIncrement); MCU_Write16(cpu,0x941c,amplitude.state().level);
            MCU_Write16(cpu,0x9408,123); MCU_Write16(cpu,0x9412,17); MCU_Write(cpu,0x9460,30); MCU_Write(cpu,0x9461,40);
            MCU_Write(cpu,0x944f,10); MCU_Write(cpu,0x93f8,20); MCU_Write(cpu,0x9453,55); MCU_Write(cpu,0x93fc,66);
            MCU_Write16(cpu,0x940a,456); MCU_Write16(cpu,0x9414,7); MCU_Write(cpu,0x944a,22); MCU_Write(cpu,0x944e,33);
            MCU_Write16(cpu,0x9420,4444); MCU_Write16(cpu,0x9454,5555); MCU_Write16(cpu,0x9456,6666); MCU_Write16(cpu,0x945e,7777);
            put24(0x942c,0x9444,60000); put24(0x946a,0x9470,12345); put24(0x946b,0x9472,65000); put24(0x946f,0x947a,55000);
            MCU_Write16(cpu,0x940c,321); MCU_Write16(cpu,0x9416,9); MCU_Write16(cpu,0x947c,32768); MCU_Write16(cpu,0x9484,8192); MCU_Write(cpu,0x93fd,0);
            MCU_Write16(cpu,0x9390,aux.modulationReleaseWords[0]); MCU_Write16(cpu,0x93b2,aux.modulationReleaseWords[1]);
            cpu.r[0] = 0x9400; execute(0x3212,0x32f0,0x3363);
            const auto action = sc55::ConsumeVoiceRelease(aux,amplitude,pitch);
            const auto& a = amplitude.state().segment;
            const auto& s = aux.second;
            if (aux.pending != MCU_Read(cpu,0xac2a) || aux.activity != MCU_Read(cpu,0xac42)
                || uint16_t(a.stage == sc55::EnvelopeStage::finished ? 22 : unsigned(a.stage)*2) != word(0x9400)
                || a.progress.position != word(0x9408) || a.progress.deferredTicks != word(0x9412)
                || a.start != MCU_Read(cpu,0x9460) || a.target != MCU_Read(cpu,0x9461)
                || s.stage != word(0x9402) || s.progress.position != word(0x940a) || s.progress.deferredTicks != word(0x9414)
                || s.parameter != MCU_Read(cpu,0x944a) || s.start != word(0x9454) || s.target != word(0x9456)
                || pitch.stage != word(0x9404) || pitch.segment.progress.position != word(0x940c) || pitch.segment.progress.deferredTicks != word(0x9416)
                || aux.modulationReleaseWords[0] != word(0x9390) || aux.modulationReleaseWords[1] != word(0x93b2)
                || (action && *action == sc55::VoiceReleaseAction::cancel) != (cpu.pc == 0x3363))
                throw std::runtime_error("Composed voice release differs");
            ++voiceReleaseChecks;
        }
    std::printf("Native composed voice release: %u H8 cases matched\n",voiceReleaseChecks);
    unsigned releaseGateChecks = 0;
    for (unsigned firstStage = 0; firstStage < 32; ++firstStage)
        for (unsigned pitchStage = 0; pitchStage <= 22; pitchStage += 2)
            for (uint16_t delay : {uint16_t(0),uint16_t(1),uint16_t(65535)})
            {
                sc55::PitchEnvelopeRunner state;
                state.stage = uint16_t(pitchStage); state.output = 60000;
                state.segment = {{123,17},12345,65000,32768,0};
                state.releaseTarget = 55000; state.releaseIncrement = 8192;
                MCU_Write16(cpu,0x9400,uint16_t(firstStage)); MCU_Write16(cpu,0x9404,uint16_t(pitchStage));
                MCU_Write16(cpu,0x9410,delay); MCU_Write16(cpu,0x93fe,0);
                MCU_Write(cpu,0x942c,0); MCU_Write16(cpu,0x9444,60000);
                MCU_Write(cpu,0x946a,0); MCU_Write16(cpu,0x9470,12345);
                MCU_Write(cpu,0x946b,0); MCU_Write16(cpu,0x9472,65000);
                MCU_Write(cpu,0x946f,0); MCU_Write16(cpu,0x947a,55000);
                MCU_Write16(cpu,0x9484,8192); MCU_Write16(cpu,0x947c,32768); MCU_Write(cpu,0x93fd,0);
                MCU_Write16(cpu,0x940c,123); MCU_Write16(cpu,0x9416,17);
                cpu.r[0] = 0x9400; execute(0x3220,0x32f0,0x3363);
                const auto action = state.requestRelease(uint16_t(firstStage),delay);
                const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                const auto pitch = [&](unsigned hi,unsigned lo) { return (uint32_t(MCU_Read(cpu,hi))<<16)|word(lo); };
                if (state.stage != word(0x9404) || state.segment.start != pitch(0x946a,0x9470)
                    || state.segment.target != pitch(0x946b,0x9472) || state.segment.increment != word(0x947c)
                    || state.segment.direction != MCU_Read(cpu,0x93fd) || state.segment.progress.position != word(0x940c)
                    || state.segment.progress.deferredTicks != word(0x9416)
                    || (action == sc55::VoiceReleaseAction::cancel) != (cpu.pc == 0x3363))
                    throw std::runtime_error("Pitch release request gate differs");
                ++releaseGateChecks;
            }
    std::printf("Native pitch release request gate: %u H8 cases matched\n",releaseGateChecks);
    for (bool reentry : {false,true})
    for (unsigned stage = 0; stage <= 22; stage += 2)
        for (unsigned variant = 0; variant < 16; ++variant)
        {
            sc55::PitchEnvelopeRunner runner;
            runner.stage = uint16_t(stage);
            runner.segment = {{uint16_t(variant%2 ? 65535 : 0),uint16_t(variant*17)},60000,61000,uint16_t(variant*4369),uint8_t(variant%3)};
            runner.nextTargets = {59000,62000,60000}; runner.nextIncrements = {65535,32768,8192}; runner.output = 12345;
            runner.releaseTarget = variant%2 ? 65000 : 55000;
            runner.releaseIncrement = uint16_t(variant*4369);
            const auto put24 = [&](unsigned hi,unsigned lo,uint32_t v) { MCU_Write(cpu,hi,uint8_t(v>>16)); MCU_Write16(cpu,lo,uint16_t(v)); };
            const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
            const auto pitch = [&](unsigned hi,unsigned lo) { return (uint32_t(MCU_Read(cpu,hi))<<16)|word(lo); };
            MCU_Write16(cpu,0x9404,runner.stage); MCU_Write16(cpu,0x940c,runner.segment.progress.position); MCU_Write16(cpu,0x9416,runner.segment.progress.deferredTicks);
            MCU_Write16(cpu,0x947c,runner.segment.increment); MCU_Write(cpu,0x93fd,runner.segment.direction);
            put24(0x946a,0x9470,60000); put24(0x946b,0x9472,61000);
            put24(0x942c,0x9444,runner.output); put24(0x942d,0x9446,runner.output);
            put24(0x946f,0x947a,runner.releaseTarget); MCU_Write16(cpu,0x9484,runner.releaseIncrement);
            for (unsigned i = 0; i < 3; ++i) { put24(0x946c + i,0x9474 + 2*i,runner.nextTargets[i]); MCU_Write16(cpu,0x947e + 2*i,runner.nextIncrements[i]); }
            for (unsigned tick = 0; tick < 64; ++tick)
            {
                const uint16_t elapsed = uint16_t(tick%3 == 0 ? 65535 : tick%3 == 1 ? 0 : 1);
                const bool entering = reentry && tick%4 == 0;
                MCU_Write16(cpu,0xac5a,elapsed); cpu.r[0] = 0x9400; execute(entering ? 0x4f9e : 0x4fdb,0x50cf,0x5367);
                const auto result = entering ? runner.reenter(elapsed) : runner.advance(elapsed);
                if (result == sc55::PitchEnvelopeRunner::Result::invalidStage
                    || (result == sc55::PitchEnvelopeRunner::Result::idle) != (cpu.pc == 0x5367)
                    || runner.stage != word(0x9404) || runner.output != pitch(0x942d,0x9446) || runner.output != pitch(0x942c,0x9444)
                    || runner.segment.start != pitch(0x946a,0x9470) || runner.segment.target != pitch(0x946b,0x9472)
                    || runner.segment.increment != word(0x947c) || runner.segment.direction != MCU_Read(cpu,0x93fd)
                    || runner.segment.progress.position != word(0x940c) || runner.segment.progress.deferredTicks != word(0x9416))
                    throw std::runtime_error("Persistent pitch envelope runner differs");
                ++pitchRunnerChecks;
            }
        }
    std::printf("Native pitch envelope stage runner: %u persistent H8 updates matched\n",pitchRunnerChecks);
    unsigned pitchSegmentChecks = 0;
    unsigned pitchRetargetChecks = 0;
    for (uint32_t previous : {0u,1u,65535u,65536u,60000u,0x7fffffu,0xffffffu})
        for (uint32_t next : {0u,1u,65535u,65536u,60000u,0x7fffffu,0xffffffu})
            for (uint16_t increment : {uint16_t(0),uint16_t(1),uint16_t(32768),uint16_t(65535)})
            {
                sc55::PitchEnvelopeSegment segment{{0,7},123,previous,17,2};
                MCU_Write(cpu,0x946b,uint8_t(previous>>16)); MCU_Write16(cpu,0x9472,uint16_t(previous));
                MCU_Write16(cpu,0x940c,0); MCU_Write16(cpu,0x9416,7);
                cpu.r[0] = 0x9400; cpu.r[4] = increment; cpu.r[5] = uint16_t(next>>16); cpu.r[6] = uint16_t(next);
                execute(0x5019,0x5040);
                segment.retarget(next,increment);
                const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                const auto pitch = [&](unsigned hi,unsigned lo) { return (uint32_t(MCU_Read(cpu,hi))<<16)|word(lo); };
                if (segment.start != pitch(0x946a,0x9470) || segment.target != pitch(0x946b,0x9472)
                    || segment.direction != MCU_Read(cpu,0x93fd) || segment.increment != word(0x947c))
                    throw std::runtime_error("Pitch segment retarget differs");
                MCU_Write16(cpu,0xac5a,1); execute(0x5060,0x50cf);
                if (segment.advance(1) != pitch(0x942d,0x9446)
                    || segment.progress.position != word(0x940c) || segment.progress.deferredTicks != word(0x9416))
                    throw std::runtime_error("Pitch retarget and advance differ");
                ++pitchRetargetChecks;
            }
    std::printf("Native pitch segment retarget and advance: %u H8 cases matched\n",pitchRetargetChecks);
    for (unsigned variant = 0; variant < 64; ++variant)
    {
        sc55::PitchEnvelopeSegment segment{{uint16_t(variant*1041),uint16_t(variant*997)},
            (variant*0x34567u)&0xffffff,(variant*0x45678u)&0xffffff,uint16_t(variant*1041),uint8_t(variant%3)};
        const auto put24 = [&](unsigned hi,unsigned lo,uint32_t v) { MCU_Write(cpu,hi,uint8_t(v>>16)); MCU_Write16(cpu,lo,uint16_t(v)); };
        put24(0x946a,0x9470,segment.start); put24(0x946b,0x9472,segment.target);
        MCU_Write16(cpu,0x940c,segment.progress.position); MCU_Write16(cpu,0x9416,segment.progress.deferredTicks);
        MCU_Write16(cpu,0x947c,segment.increment); MCU_Write(cpu,0x93fd,segment.direction);
        for (unsigned tick = 0; tick < 128; ++tick)
        {
            const uint16_t elapsed = uint16_t(tick%4 == 0 ? 65535 : tick%4 == 1 ? 0 : tick%4 == 2 ? 1 : 17);
            MCU_Write16(cpu,0xac5a,elapsed); cpu.r[0] = 0x9400; execute(0x5060,0x50cf);
            const auto actualWord = [&](unsigned at) { return uint16_t((MCU_Read(cpu,at)<<8)|MCU_Read(cpu,at+1)); };
            const auto result = segment.advance(elapsed);
            if (segment.progress.position != actualWord(0x940c) || segment.progress.deferredTicks != actualWord(0x9416)
                || result != ((uint32_t(MCU_Read(cpu,0x942c))<<16)|actualWord(0x9444))
                || result != ((uint32_t(MCU_Read(cpu,0x942d))<<16)|actualWord(0x9446)))
                throw std::runtime_error("Persistent pitch envelope segment differs");
            ++pitchSegmentChecks;
        }
    }
    std::printf("Native persistent pitch envelope segment: %u H8 updates matched\n",pitchSegmentChecks);
    std::array<uint8_t,256> pitchEnvelopeCurve;
    sc55::PitchEnvelopeDepthTables pitchDepthTables;
    const auto romWord = [&](unsigned at) { return uint16_t((MCU_Read(cpu,at)<<8)|MCU_Read(cpu,at+1)); };
    sc55::EnvelopeTimes pitchTimes;
    for (unsigned i = 0; i < 128; ++i) pitchTimes[i] = romWord(0x6f12+2*i);
    unsigned secondTimingChecks = 0;
    unsigned secondRunnerChecks = 0;
    unsigned secondInitializationChecks = 0;
    for (unsigned stage = 0; stage <= 22; stage += 2)
        for (unsigned variant = 0; variant < 4; ++variant)
        {
            sc55::SecondEnvelopeReleaseState state;
            state.stage = uint16_t(stage); state.parameter = 32;
            state.start = 50000; state.target = 19000; state.level = 123;
            state.progress = {uint16_t(variant%2 ? 65535 : 0),17};
            state.nextParameters = {16,48,64}; state.nextTargets = {10000,55000,12000};
            sc55::SecondEnvelopeTiming timing;
            uint16_t pcmLevel = 12345;
            sc55::SecondEnvelopePcmState pcmState{8,0,pcmLevel,0xff00};
            MCU_Write(cpu,0x9468,8); MCU_Write16(cpu,0x9422,0); MCU_Write16(cpu,0x9426,0xff00);
            MCU_Write16(cpu,0x9402,state.stage); MCU_Write16(cpu,0x940a,state.progress.position); MCU_Write16(cpu,0x9414,17);
            MCU_Write16(cpu,0x9454,50000); MCU_Write16(cpu,0x9456,19000); MCU_Write16(cpu,0x9420,123); MCU_Write16(cpu,0x9424,pcmLevel); MCU_Write16(cpu,0x93d0,0);
            MCU_Write(cpu,0x944a,32); MCU_Write16(cpu,0x93fe,0); MCU_Write16(cpu,0x942e,0x9600); MCU_Write(cpu,0x94a2,16);
            for (unsigned i = 0; i < 3; ++i) { MCU_Write(cpu,0x944b+i,state.nextParameters[i]); MCU_Write16(cpu,0x9458+2*i,state.nextTargets[i]); MCU_Write(cpu,0x9614+i,64); }
            for (unsigned a : {0x93d4u,0x93d6u,0x93d8u,0x93dau}) MCU_Write16(cpu,a,256);
            timing.attackControlEnabled = true;
            for (unsigned tick = 0; tick < 32; ++tick)
            {
                const bool bypass = variant >= 2 && tick%3 == 0;
                const uint16_t elapsed = uint16_t(tick%4 == 0 ? 65535 : tick%4);
                sc55::SecondEnvelopeOutputInputs outputInput;
                outputInput.base = uint8_t((tick*17)%128); outputInput.control = uint8_t((tick*31)%128);
                outputInput.offset = uint16_t(tick*313);
                outputInput.sources[0] = {uint16_t(tick*257),uint16_t(tick*519),uint16_t(tick*2047)};
                outputInput.sources[1] = {uint16_t(tick*977),uint16_t(tick*41),uint16_t(tick*1023)};
                const auto base = uint8_t((tick*7)%128), controller = uint8_t((tick*13)%128), limit = uint8_t(127);
                MCU_Write(cpu,0x94a3,outputInput.base); MCU_Write(cpu,0x9612,outputInput.control); MCU_Write16(cpu,0x9488,outputInput.offset);
                MCU_Write16(cpu,0x9388,outputInput.sources[0].first); MCU_Write16(cpu,0x948c,outputInput.sources[0].second); MCU_Write16(cpu,0x93a0,outputInput.sources[0].waveform);
                MCU_Write16(cpu,0x93aa,outputInput.sources[1].first); MCU_Write16(cpu,0x9494,outputInput.sources[1].second); MCU_Write16(cpu,0x93c2,outputInput.sources[1].waveform);
                MCU_Write(cpu,0x9467,base); MCU_Write(cpu,0x9613,controller); MCU_Write(cpu,0x9469,limit);
                if (variant%2 && tick%8 == 0)
                {
                    MCU_Write16(cpu,0xac5a,elapsed); cpu.r[0] = 0x9400; cpu.r[7] = 0x9000;
                    execute(0x4403,0x4435);
                    const auto activation = sc55::InitializeSecondEnvelope(state,pcmState,timing,pitchTimes,outputInput,base,controller,limit,secondPcmTables);
                    if (!activation || activation->level != romWord(0x93ea) || activation->command != romWord(0x93e8)
                        || state.stage != romWord(0x9402) || state.level != romWord(0x9420)
                        || state.progress.position != romWord(0x940a) || state.progress.deferredTicks != romWord(0x9414)
                        || state.stepTime != romWord(0x93d0) || pcmState.level != romWord(0x9424)
                        || pcmState.output != romWord(0x9422) || pcmState.command != romWord(0x9426)
                        || pcmState.control != MCU_Read(cpu,0x9468) || romWord(0xac5a) != elapsed || cpu.r[7] != 0x9000)
                        throw std::runtime_error("Second envelope initialization differs");
                    pcmLevel = pcmState.level;
                    ++secondInitializationChecks;
                }
                auto integratedState = state;
                const auto integratedResult = sc55::AdvanceSecondEnvelope(integratedState,pcmState,bypass,elapsed,timing,pitchTimes,outputInput,base,controller,limit,secondPcmTables);
                MCU_Write(cpu,0x9465,bypass ? 1 : 0); MCU_Write16(cpu,0xac5a,elapsed); cpu.r[0] = 0x9400;
                execute(0x4443,bypass ? 0x4448 : 0x4662,0x47fa);
                const auto result = state.advance(bypass,elapsed,timing,pitchTimes,pcmLevel);
                if (result == sc55::SecondEnvelopeReleaseState::Result::invalidInput
                    || (result == sc55::SecondEnvelopeReleaseState::Result::idle) != (cpu.pc != 0x4662)
                    || state.stage != romWord(0x9402) || state.parameter != MCU_Read(cpu,0x944a)
                    || state.start != romWord(0x9454) || state.target != romWord(0x9456) || state.level != romWord(0x9420)
                    || state.progress.position != romWord(0x940a) || state.progress.deferredTicks != romWord(0x9414)
                    || state.stepTime != romWord(0x93d0) || pcmLevel != romWord(0x9424))
                    throw std::runtime_error("Second envelope dispatcher differs");
                if (result == sc55::SecondEnvelopeReleaseState::Result::updated)
                {
                    cpu.r[7] = 0x9000; MCU_Write16(cpu,0x9000,0x7000);
                    execute(0x4662,0x7000);
                }
                if (integratedResult != result || integratedState.stage != state.stage
                    || integratedState.progress.position != state.progress.position || integratedState.level != state.level
                    || pcmState.output != romWord(0x9422) || pcmState.level != romWord(0x9424)
                    || pcmState.command != romWord(0x9426) || pcmState.control != MCU_Read(cpu,0x9468))
                    throw std::runtime_error("Integrated second envelope differs");
                pcmLevel = pcmState.level;
                ++secondRunnerChecks;
            }
        }
    std::printf("Native second envelope dispatcher: %u persistent H8 updates matched\n",secondRunnerChecks);
    std::printf("Native second envelope initialization: %u H8 cases matched\n",secondInitializationChecks);
    cpu.r[7] = savedStack;
    for (unsigned parameter = 0; parameter < 128; ++parameter)
        for (unsigned control = 0; control < 128; ++control)
            for (unsigned mode = 0; mode < 4; ++mode)
            {
                sc55::SecondEnvelopeTiming timing;
                timing.keyScale = uint16_t(parameter*521); timing.releaseKeyScale = uint16_t(control*631);
                timing.attackVelocityScale = uint16_t(control*257); timing.decayReleaseVelocityScale = uint16_t(parameter*313);
                timing.attack = timing.decay = timing.release = uint8_t(control); timing.attackControlEnabled = mode == 1;
                const auto interval = mode < 2 ? sc55::SecondEnvelopeInterval::attack : mode == 2 ? sc55::SecondEnvelopeInterval::decay : sc55::SecondEnvelopeInterval::release;
                MCU_Write(cpu,0x944a,uint8_t(parameter)); MCU_Write(cpu,0x94a2,mode == 1 ? 16 : 0);
                MCU_Write16(cpu,0x942e,0x9600); MCU_Write(cpu,0x9614,uint8_t(control)); MCU_Write(cpu,0x9615,uint8_t(control)); MCU_Write(cpu,0x9616,uint8_t(control));
                MCU_Write16(cpu,0x93d4,timing.keyScale); MCU_Write16(cpu,0x93d6,timing.releaseKeyScale);
                MCU_Write16(cpu,0x93d8,timing.attackVelocityScale); MCU_Write16(cpu,0x93da,timing.decayReleaseVelocityScale);
                cpu.r[0] = 0x9400; execute(mode < 2 ? 0x44a3 : mode == 2 ? 0x44ff : 0x4564,0x45b6);
                const auto duration = timing.duration(uint8_t(parameter),interval,pitchTimes);
                if (!duration || *duration != cpu.r[6]) throw std::runtime_error("Second envelope duration differs");
                sc55::SecondEnvelopeReleaseState state;
                state.parameter = uint8_t(parameter); state.start = 50000; state.target = 19000; state.progress = {123,17};
                MCU_Write16(cpu,0x9454,50000); MCU_Write16(cpu,0x9456,19000); MCU_Write16(cpu,0x940a,123); MCU_Write16(cpu,0x9414,17);
                MCU_Write16(cpu,0xac5a,uint16_t(control)); execute(0x45b6,0x4662);
                if (!state.advanceInterval(interval,timing,pitchTimes,uint16_t(control))
                    || state.level != romWord(0x9420) || state.stepTime != romWord(0x93d0)
                    || state.progress.position != romWord(0x940a) || state.progress.deferredTicks != romWord(0x9414))
                    throw std::runtime_error("Second envelope timed segment differs");
                ++secondTimingChecks;
            }
    std::printf("Native second envelope timed segment: %u H8 cases matched\n",secondTimingChecks);
    unsigned pitchIncrementChecks = 0;
    for (unsigned sensitivity = 44; sensitivity <= 84; ++sensitivity)
        for (unsigned velocity = 0; velocity < 256; ++velocity)
            for (uint8_t parameter : {uint8_t(0),uint8_t(1),uint8_t(63),uint8_t(127),uint8_t(128),uint8_t(129),uint8_t(191),uint8_t(255)})
                for (uint16_t keyScale : {uint16_t(0),uint16_t(1),uint16_t(256),uint16_t(65535)})
                {
                    std::array<uint8_t,92> partial{}; partial[0x23] = uint8_t(sensitivity); partial[0x17] = parameter;
                    MCU_Write(cpu,0x9623,uint8_t(sensitivity)); MCU_Write(cpu,0x9617,parameter);
                    MCU_Write(cpu,0xc914,uint8_t(velocity)); MCU_Write16(cpu,0x93ca,keyScale);
                    cpu.r[0] = 0x9400; cpu.r[1] = 0; cpu.r[5] = 0x9600; execute(0x4d8c,0x4e2a);
                    const auto increment = sc55::PreparePitchEnvelopeFirstIncrement(partial,uint8_t(velocity),keyScale,pitchTimes);
                    if (!increment || *increment != romWord(0x947c)
                        || sc55::EnvelopeVelocityScale(uint8_t(velocity),uint8_t(sensitivity)) != romWord(0x93ce))
                        throw std::runtime_error("Pitch envelope first increment differs");
                    ++pitchIncrementChecks;
                }
    std::printf("Native pitch envelope velocity and first increment: %u H8 cases matched\n",pitchIncrementChecks);
    sc55::PitchEnvelopeKeyCurves pitchKeyCurves;
    std::array<uint16_t,256> keyMultipliers;
    for (unsigned i = 0; i < 256; ++i) keyMultipliers[i] = romWord(0x67c6+2*i);
    for (unsigned curve = 0; curve < 16; ++curve)
        for (unsigned key = 0; key < 256; ++key)
        {
            pitchKeyCurves.attack[curve][key] = MCU_Read(cpu,0x30000|uint16_t(romWord(0x3dd42+2*curve)+key));
            pitchKeyCurves.release[curve][key] = MCU_Read(cpu,0x30000|uint16_t(romWord(0x3dd62+2*curve)+key));
        }
    unsigned pitchKeyScaleChecks = 0;
    for (unsigned curve = 0; curve < 16; ++curve)
        for (unsigned key = 0; key < 256; ++key)
            for (unsigned sensitivity = 44; sensitivity <= 84; ++sensitivity)
            {
                std::array<uint8_t,92> partial{};
                partial[0x1e] = uint8_t(curve|((key&15)<<4)); partial[0x1f] = uint8_t(15-curve);
                partial[0x20] = uint8_t(sensitivity); partial[0x21] = uint8_t(128-sensitivity);
                partial[0x23] = uint8_t(44+(key%41));
                for (unsigned i = 0; i < 5; ++i) partial[0x17+i] = uint8_t(key+63*i);
                for (unsigned i = 0; i < partial.size(); ++i) MCU_Write(cpu,0x9600+i,partial[i]);
                MCU_Write16(cpu,0x93fe,0); MCU_Write(cpu,0xc8fc,uint8_t(key));
                MCU_Write(cpu,0xc914,uint8_t(255-key));
                cpu.r[0] = 0x9400; cpu.r[5] = 0x9600; execute(0x4cbb,0x4f51);
                const auto result = sc55::PreparePitchEnvelopeKeyScales(partial,uint8_t(key),pitchKeyCurves,keyMultipliers);
                if (!result || (*result)[0] != romWord(0x93ca) || (*result)[1] != romWord(0x93cc))
                    throw std::runtime_error("Pitch envelope key scales differ");
                const auto timing = sc55::PreparePitchEnvelopeTiming(partial,uint8_t(key),uint8_t(255-key),pitchKeyCurves,keyMultipliers,pitchTimes);
                if (!timing || timing->velocityScale != romWord(0x93ce)) throw std::runtime_error("Pitch envelope timing scale differs");
                for (unsigned i = 0; i < 5; ++i)
                    if (timing->increments[i] != romWord(0x947c+2*i)) throw std::runtime_error("Pitch envelope timing increment differs");
                ++pitchKeyScaleChecks;
            }
    std::printf("Native pitch envelope complete timing: %u H8 cases matched\n",pitchKeyScaleChecks);
    for (unsigned i = 0; i < 192; ++i)
    { pitchDepthTables.base[i] = romWord(0x78c6+2*i); pitchDepthTables.velocity[i] = romWord(0x78dc+2*i); }
    for (unsigned i = 0; i < 256; ++i) pitchDepthTables.depth[i] = romWord(0x78f2+2*i);
    for (unsigned i = 0; i < 256; ++i) pitchEnvelopeCurve[i] = MCU_Read(cpu,0x79f2+i);
    unsigned pitchDepthChecks = 0;
    unsigned installedPitchChecks = 0;
    for (unsigned variant = 0; variant < 512; ++variant)
    {
        std::array<uint8_t,92> partial{};
        partial[0x10] = uint8_t(variant); partial[0x11] = uint8_t(variant*3);
        partial[0x22] = uint8_t(variant/2); partial[0x23] = uint8_t(44+variant%41);
        partial[0x1e] = uint8_t(variant%16); partial[0x1f] = uint8_t((variant/16)%16);
        partial[0x20] = uint8_t(44+variant%41); partial[0x21] = uint8_t(84-variant%41);
        for (unsigned i = 0; i < 5; ++i) { partial[0x12+i] = uint8_t(variant+i*31); partial[0x17+i] = uint8_t(variant+i*17); }
        for (unsigned i = 0; i < partial.size(); ++i) MCU_Write(cpu,0x9600+i,partial[i]);
        const uint8_t key = uint8_t(variant), velocity = uint8_t(255-key), cached = uint8_t(variant*7);
        MCU_Write(cpu,0xc914,velocity); MCU_Write(cpu,0xc8fc,key); MCU_Write16(cpu,0x93fe,0);
        MCU_Write(cpu,0x9428,0); MCU_Write16(cpu,0x943c,60000); MCU_Write(cpu,0x93c4,cached);
        cpu.r[0] = 0x9400; cpu.r[5] = 0x9600;
        execute(0x4a5a,0x4f51);
        const auto targets = sc55::PreparePitchEnvelopeTargets(60000,partial,velocity,cached,pitchDepthTables,pitchEnvelopeCurve);
        const auto timing = sc55::PreparePitchEnvelopeTiming(partial,key,velocity,pitchKeyCurves,keyMultipliers,pitchTimes);
        if (!timing) throw std::runtime_error("Pitch installation timing rejected");
        sc55::VoicePitchRunner state;
        state.envelope.stage = 12; state.envelope.segment.progress = {123,17};
        state.glide.increment = 456; state.glide.pitch.correction = {128,25};
        state.installEnvelope(60000,targets,*timing);
        const auto pitch = [&](unsigned hi,unsigned lo) { return (uint32_t(MCU_Read(cpu,hi))<<16)|romWord(lo); };
        if (state.envelope.segment.start != pitch(0x946a,0x9470) || state.envelope.segment.target != pitch(0x946b,0x9472)
            || state.envelope.segment.direction != MCU_Read(cpu,0x93fd) || state.envelope.segment.increment != romWord(0x947c)
            || state.envelope.releaseTarget != pitch(0x946f,0x947a) || state.envelope.releaseIncrement != romWord(0x9484)
            || state.envelope.output != pitch(0x942c,0x9444) || state.glide.pitch.accumulator != pitch(0x942d,0x9446)
            || state.envelope.stage != 12 || state.envelope.segment.progress.position != 123 || state.envelope.segment.progress.deferredTicks != 17
            || state.glide.increment != 456 || state.glide.pitch.correction.source != 128 || state.glide.pitch.correction.offset != 25)
            throw std::runtime_error("Prepared pitch envelope installation differs");
        for (unsigned i = 0; i < 3; ++i)
            if (state.envelope.nextTargets[i] != pitch(0x946c + i,0x9474 + 2*i) || state.envelope.nextIncrements[i] != romWord(0x947e + 2*i))
                throw std::runtime_error("Prepared pitch future segment differs");
        ++installedPitchChecks;
    }
    std::printf("Native prepared pitch envelope installation: %u H8 cases matched\n",installedPitchChecks);
    for (unsigned sensitivity = 0; sensitivity < 256; ++sensitivity)
        for (unsigned depth = 0; depth < 256; ++depth)
            for (uint8_t velocity : {uint8_t(0),uint8_t(1),uint8_t(127),uint8_t(255)})
            {
                std::array<uint8_t,92> partial{};
                partial[0x10] = uint8_t(depth); partial[0x22] = uint8_t(sensitivity);
                partial[0x11] = uint8_t(sensitivity);
                for (unsigned i = 0; i < 5; ++i) partial[0x12+i] = uint8_t(depth+i*63);
                for (unsigned i = 0; i < partial.size(); ++i) MCU_Write(cpu,0x9600+i,partial[i]);
                MCU_Write(cpu,0xc914,velocity); MCU_Write(cpu,0x9428,0); MCU_Write16(cpu,0x943c,60000);
                MCU_Write(cpu,0x93c4,uint8_t(depth+velocity));
                cpu.r[0] = 0x9400; cpu.r[1] = 0; cpu.r[5] = 0x9600;
                execute(0x4a5a,0x4ada);
                const auto result = sc55::PreparePitchEnvelopeDepth(partial,velocity,pitchDepthTables);
                constexpr std::array<unsigned,5> fields{0x6a,0x6b,0x6c,0x6d,0x6f};
                if (romWord(0x93c8) != result.depth) throw std::runtime_error("Pitch envelope depth differs");
                for (unsigned i = 0; i < fields.size(); ++i)
                    if (MCU_Read(cpu,0x9400+fields[i]) != result.offsets[i]) throw std::runtime_error("Pitch envelope offset differs");
                execute(0x4ada,0x4cb7);
                const auto targets = sc55::PreparePitchEnvelopeTargets(60000,partial,velocity,uint8_t(depth+velocity),pitchDepthTables,pitchEnvelopeCurve);
                constexpr std::array<unsigned,5> lowFields{0x70,0x72,0x74,0x76,0x7a};
                for (unsigned i = 0; i < fields.size(); ++i)
                    if (targets.pitch[i] != ((uint32_t(MCU_Read(cpu,0x9400+fields[i]))<<16)|romWord(0x9400+lowFields[i])))
                        throw std::runtime_error("Composed pitch envelope targets differ");
                if (targets.direction != MCU_Read(cpu,0x93fd)
                    || targets.pitch[0] != ((uint32_t(MCU_Read(cpu,0x942d))<<16)|romWord(0x9446))
                    || targets.pitch[0] != ((uint32_t(MCU_Read(cpu,0x942c))<<16)|romWord(0x9444)))
                    throw std::runtime_error("Pitch envelope initial state differs");
                ++pitchDepthChecks;
            }
    std::printf("Native pitch envelope depth, five targets and initial state: %u H8 cases matched\n",pitchDepthChecks);
    unsigned pitchTargetChecks = 0;
    for (unsigned offset = 0; offset < 256; ++offset)
        for (uint16_t depth : {uint16_t(0),uint16_t(1),uint16_t(32767),uint16_t(32768),uint16_t(65535)})
            for (uint32_t base : {0u,60000u,0xffffffu})
            {
                MCU_Write(cpu,0x946a,uint8_t(offset)); MCU_Write(cpu,0x9428,uint8_t(base>>16)); MCU_Write16(cpu,0x943c,uint16_t(base));
                cpu.r[0] = 0x9400; cpu.r[2] = depth; execute(0x4ada,0x4b23);
                const uint32_t actual = (uint32_t(MCU_Read(cpu,0x946a))<<16)|(MCU_Read(cpu,0x9470)<<8)|MCU_Read(cpu,0x9471);
                if (actual != sc55::PreparePitchEnvelopeTarget(base,uint8_t(offset),depth,pitchEnvelopeCurve))
                    throw std::runtime_error("Pitch envelope target differs");
                ++pitchTargetChecks;
            }
    std::printf("Native pitch envelope target: %u H8 cases matched\n",pitchTargetChecks);
    unsigned basePitchChecks = 0;
    sc55::PartPitchKeyTables pitchKeyTables{};
    for (unsigned selector = 1; selector < pitchKeyTables.size(); ++selector)
    {
        const unsigned at = 0x3dd32+selector*2;
        const unsigned pointer = (MCU_Read(cpu,at)<<8)|MCU_Read(cpu,at+1);
        for (unsigned key = 0; key < 256; ++key)
            pitchKeyTables[selector][key] = uint16_t((MCU_Read(cpu,0x30000+((pointer+key*2)&0xffff))<<8)
                |MCU_Read(cpu,0x30000+((pointer+key*2+1)&0xffff)));
    }
    unsigned keyTableChecks = 0;
    for (unsigned selector = 0; selector < pitchKeyTables.size(); ++selector)
        for (unsigned key = 0; key < 256; ++key)
            for (uint32_t initial : {0u,60000u,0xffffffu})
            {
                MCU_Write(cpu,0x9613,uint8_t(selector)); MCU_Write(cpu,0xc98c,uint8_t(key));
                MCU_Write(cpu,0x9428,uint8_t(initial>>16)); MCU_Write16(cpu,0x943c,uint16_t(initial));
                cpu.r[0] = 0x9400; cpu.r[1] = 0; cpu.r[5] = 0x9600; execute(0x48d8,0x491f);
                const uint32_t actual = (uint32_t(MCU_Read(cpu,0x9428))<<16)|(MCU_Read(cpu,0x943c)<<8)|MCU_Read(cpu,0x943d);
                if (sc55::ApplyPartPitchKeyTable(initial,uint8_t(selector),uint8_t(key),pitchKeyTables) != actual)
                    throw std::runtime_error("Owned part pitch key table differs");
                ++keyTableChecks;
            }
    std::printf("Native owned part pitch key tables: %u H8 cases matched\n",keyTableChecks);
    for (unsigned key = 0; key < 256; ++key)
        for (uint16_t tune : {uint16_t(0),uint16_t(1023),uint16_t(1024),uint16_t(65535)})
            for (uint16_t alternate : {uint16_t(0),uint16_t(1024),uint16_t(32768),uint16_t(65535)})
            {
                MCU_Write(cpu,0xc98c,uint8_t(key)); MCU_Write16(cpu,0xc9a4,tune);
                MCU_Write(cpu,0x949a,0); MCU_Write16(cpu,0x94a0,0x9600);
                MCU_Write(cpu,0x960b,uint8_t(255-key)); MCU_Write16(cpu,0x960c,tune); MCU_Write16(cpu,0x960e,alternate);
                cpu.r[0] = 0x9400; cpu.r[1] = 0; execute(0x486a,0x48d0);
                const auto result = sc55::PreparePartPitchBase(uint8_t(key),tune,uint8_t(255-key),tune,alternate);
                const auto get24 = [&](unsigned hi,unsigned lo) { return (uint32_t(MCU_Read(cpu,hi))<<16)|(MCU_Read(cpu,lo)<<8)|MCU_Read(cpu,lo+1); };
                if (result.pitch != get24(0x9428,0x943c) || result.reference != get24(0x9429,0x943e)
                    || result.alternateReference != get24(0x942a,0x9440))
                    throw std::runtime_error("Part pitch base differs");
                ++basePitchChecks;
            }
    unsigned keyCorrectionChecks = 0;
    for (uint32_t initial : {0u,300u,60000u,0x7fffffu,0xffffffu})
        for (unsigned correction = 0; correction < 65536; ++correction)
        {
            MCU_Write(cpu,0x9428,uint8_t(initial>>16)); MCU_Write16(cpu,0x943c,uint16_t(initial));
            cpu.r[0] = 0x9400; cpu.r[6] = uint16_t(correction); execute(0x48f9,0x491f);
            const uint32_t actual = (uint32_t(MCU_Read(cpu,0x9428))<<16)|(MCU_Read(cpu,0x943c)<<8)|MCU_Read(cpu,0x943d);
            if (actual != sc55::ApplyPartPitchKeyCorrection(initial,uint16_t(correction)))
                throw std::runtime_error("Part pitch key correction differs");
            ++keyCorrectionChecks;
        }
    std::printf("Native part pitch base: %u; key corrections: %u H8 cases matched\n",basePitchChecks,keyCorrectionChecks);
    unsigned partPitchChecks = 0;
    for (uint32_t initial : {0u,300u,0x7fffffu,0xffffffu})
        for (unsigned parameter = 0; parameter < 256; ++parameter)
        {
            MCU_Write(cpu,0x960b,uint8_t(parameter)); MCU_Write(cpu,0x9428,uint8_t(initial>>16)); MCU_Write16(cpu,0x943c,uint16_t(initial));
            cpu.r[0] = 0x9400; cpu.r[5] = 0x9600;
            execute(0x4927,0x4955);
            const auto get24 = [&](unsigned hi,unsigned lo) { return (uint32_t(MCU_Read(cpu,hi))<<16)|(MCU_Read(cpu,lo)<<8)|MCU_Read(cpu,lo+1); };
            if (get24(0x9428,0x943c) != sc55::ApplyPartPitchFine(initial,uint8_t(parameter)))
                throw std::runtime_error("Part pitch fine adjustment differs");
            for (uint8_t depth : {uint8_t(0),uint8_t(1),uint8_t(127),uint8_t(255)})
            {
                MCU_Write(cpu,0x960c,depth); MCU_Write(cpu,0x9428,uint8_t(initial>>16)); MCU_Write16(cpu,0x943c,uint16_t(initial));
                cpu.r[0] = 0x9400; cpu.r[5] = 0x9600; cpu.r[3] = uint16_t(parameter<<8);
                execute(0x4966,0x49aa);
                const auto result = sc55::ApplyPartPitchRandom(initial,uint8_t(parameter),depth);
                if (get24(0x9428,0x943c) != result || get24(0x946e,0x9478) != result)
                    throw std::runtime_error("Part pitch random adjustment differs: initial=" + std::to_string(initial)
                        + " random=" + std::to_string(parameter) + " depth=" + std::to_string(depth)
                        + " H8=" + std::to_string(get24(0x9428,0x943c)) + " native=" + std::to_string(result));
                ++partPitchChecks;
            }
        }
    std::printf("Native part pitch adjustments: 1024 fine, %u random H8 cases matched\n",partPitchChecks);
    unsigned glidePrepareChecks = 0;
    for (unsigned flags = 0; flags < 256; ++flags)
        for (uint8_t key : {uint8_t(0),uint8_t(60),uint8_t(254),uint8_t(255)})
            for (unsigned variant = 0; variant < 8; ++variant)
            {
                const uint32_t previous = (variant*0x345678u)&0xffffff;
                const uint32_t current = (variant*0x56789au+1)&0xffffff;
                sc55::PitchGlide glide{{uint32_t(variant*0x123456u)&0xffffff,{128,123}},uint32_t(variant*0x234567u)&0xffffff};
                const auto put24 = [&](unsigned high,unsigned low,uint32_t v) { MCU_Write(cpu,high,uint8_t(v>>16)); MCU_Write16(cpu,low,uint16_t(v)); };
                put24(0x942d,0x9446,glide.pitch.accumulator); put24(0x942b,0x9442,glide.increment);
                put24(0xc8b2,0xc8ae,previous); put24(0x9428,0x943c,current);
                MCU_Write16(cpu,0x93fe,uint16_t(variant)); MCU_Write(cpu,0x93c5,uint8_t(flags)); MCU_Write(cpu,0xc974+variant,key);
                cpu.r[0] = 0x9400; execute(0x49aa,0x4a47);
                glide.prepare(uint8_t(flags),key,previous,current);
                const auto get24 = [&](unsigned high,unsigned low) { return (uint32_t(MCU_Read(cpu,high))<<16)|(MCU_Read(cpu,low)<<8)|MCU_Read(cpu,low+1); };
                if (glide.increment != get24(0x942b,0x9442) || glide.pitch.accumulator != get24(0x942d,0x9446))
                    throw std::runtime_error("Glide preparation differs");
                ++glidePrepareChecks;
            }
    std::printf("Native glide preparation: %u H8 cases matched\n",glidePrepareChecks);
    sc55::PitchGlideRates glideRates;
    for (unsigned i = 0; i < glideRates.size(); ++i)
        glideRates[i] = uint16_t((MCU_Read(cpu,0x7a32+i*2)<<8)|MCU_Read(cpu,0x7a33+i*2));
    unsigned glideChecks = 0;
    unsigned voicePitchChecks = 0;
    for (unsigned variant = 0; variant < 48; ++variant)
    {
        sc55::VoicePitchRunner state;
        state.envelope.stage = uint16_t(2*(variant%12));
        state.envelope.segment = {{0,3},60000,61000,32768,0};
        state.envelope.nextTargets = {59000,62000,60000};
        state.envelope.nextIncrements = {8192,32768,65535};
        state.envelope.releaseTarget = 55000; state.envelope.releaseIncrement = 8192;
        state.envelope.output = 12345;
        state.glide = {{12345,{128,0}},variant%2 ? 6000u : 0xffe890u};
        state.pcmWord = 42;
        const auto put24 = [&](unsigned hi,unsigned lo,uint32_t v) { MCU_Write(cpu,hi,uint8_t(v>>16)); MCU_Write16(cpu,lo,uint16_t(v)); };
        const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
        const auto pitch = [&](unsigned hi,unsigned lo) { return (uint32_t(MCU_Read(cpu,hi))<<16)|word(lo); };
        MCU_Write16(cpu,0x9404,state.envelope.stage); MCU_Write16(cpu,0x940c,0); MCU_Write16(cpu,0x9416,3);
        MCU_Write16(cpu,0x947c,32768); MCU_Write(cpu,0x93fd,0);
        put24(0x946a,0x9470,60000); put24(0x946b,0x9472,61000);
        for (unsigned i = 0; i < 3; ++i) { put24(0x946c + i,0x9474 + 2*i,state.envelope.nextTargets[i]); MCU_Write16(cpu,0x947e + 2*i,state.envelope.nextIncrements[i]); }
        put24(0x946f,0x947a,55000); MCU_Write16(cpu,0x9484,8192);
        put24(0x942c,0x9444,12345); put24(0x942d,0x9446,12345); MCU_Write16(cpu,0x9448,42);
        put24(0x942b,0x9442,state.glide.increment); put24(0x9429,0x943e,81000);
        MCU_Write16(cpu,0x93c6,0x9610); MCU_Write16(cpu,0x942e,0x9600);
        MCU_Write16(cpu,0x93fe,0); MCU_Write(cpu,0xc8e4,0);
        MCU_Write(cpu,0x94a4,128); MCU_Write16(cpu,0x94a6,0);
        for (unsigned tick = 0; tick < 64; ++tick)
        {
            const uint16_t elapsed = uint16_t(tick%4 == 0 ? 65535 : tick%4);
            const bool reentry = tick%8 == 0;
            const bool initializing = variant >= 24 && tick%16 == 0;
            const uint8_t source = uint8_t(tick*7), rate = uint8_t((variant+tick)%128);
            sc55::PitchModulationInputs input;
            input.offset = uint16_t(tick*127);
            input.sources[0] = {1000,2000,uint16_t(tick*1023)};
            input.sources[1] = {65500,300,uint16_t(tick*513)};
            input.masterTune = uint16_t(1000+tick); input.partTune = uint16_t(0u-tick);
            MCU_Write16(cpu,0xac5a,elapsed); MCU_Write(cpu,0x9610,rate); MCU_Write(cpu,0x9607,source);
            MCU_Write(cpu,0xab26,rate); MCU_Write(cpu,0x93c5,128);
            MCU_Write16(cpu,0x9486,input.offset);
            MCU_Write16(cpu,0x938a,input.sources[0].first); MCU_Write16(cpu,0x9490,input.sources[0].second); MCU_Write16(cpu,0x93a0,input.sources[0].waveform);
            MCU_Write16(cpu,0x93ac,input.sources[1].first); MCU_Write16(cpu,0x9492,input.sources[1].second); MCU_Write16(cpu,0x93c2,input.sources[1].waveform);
            MCU_Write16(cpu,0x8000,input.masterTune); MCU_Write16(cpu,0xab76,input.partTune);
            cpu.r[0] = 0x9400; cpu.r[7] = 0x9000;
            if (variant%3 == 0 && tick%17 == 0)
            {
                execute(0x3269,0x326e); execute(0x32a3,0x32dc);
                state.envelope.release();
                if (state.envelope.stage != word(0x9404)
                    || state.envelope.segment.start != pitch(0x946a,0x9470)
                    || state.envelope.segment.target != pitch(0x946b,0x9472)
                    || state.envelope.segment.increment != word(0x947c)
                    || state.envelope.segment.direction != MCU_Read(cpu,0x93fd)
                    || word(0x940c) != 0 || word(0x9416) != 0)
                    throw std::runtime_error("Pitch release setup differs");
            }
            execute(initializing ? 0x4f51 : reentry ? 0x4f9e : 0x4fdb,initializing ? 0x4f90 : 0x5367);
            const auto result = initializing ? state.initialize(input,rate,glideRates,81000,source,pitchConversion)
                : state.advance(elapsed,reentry,input,rate,glideRates,81000,source,pitchConversion);
            if (initializing && (word(0xac5a) != elapsed || word(0x93c6) != 0xab26 || cpu.r[7] != 0x9000))
                throw std::runtime_error("Pitch initialization scheduler context differs");
            if (result == sc55::VoicePitchRunner::Result::invalidInput
                || state.envelope.stage != word(0x9404) || state.envelope.output != pitch(0x942c,0x9444)
                || state.envelope.segment.progress.position != word(0x940c) || state.envelope.segment.progress.deferredTicks != word(0x9416)
                || state.envelope.segment.start != pitch(0x946a,0x9470) || state.envelope.segment.target != pitch(0x946b,0x9472)
                || state.glide.increment != pitch(0x942b,0x9442) || state.glide.pitch.accumulator != pitch(0x942d,0x9446)
                || state.glide.pitch.correction.source != MCU_Read(cpu,0x94a4) || uint16_t(state.glide.pitch.correction.offset) != word(0x94a6)
                || state.pcmWord != word(0x9448))
                throw std::runtime_error("Complete voice pitch update differs at " + std::to_string(variant) + ":" + std::to_string(tick));
            ++voicePitchChecks;
        }
    }
    cpu.r[7] = savedStack;
    std::printf("Native complete voice pitch: %u persistent H8 updates matched\n",voicePitchChecks);
    for (unsigned rateIndex = 0; rateIndex < 128; ++rateIndex)
        for (uint32_t increment : {0u,60000u,0xff15a0u})
            for (uint16_t elapsed : {uint16_t(0),uint16_t(1),uint16_t(256),uint16_t(65535)})
            {
                sc55::PitchGlide glide{{81000,{128,0}},increment};
                MCU_Write16(cpu,0x9442,uint16_t(increment)); MCU_Write(cpu,0x942b,uint8_t(increment>>16));
                MCU_Write16(cpu,0x93c6,0x9610); MCU_Write(cpu,0x9610,uint8_t(rateIndex));
                MCU_Write16(cpu,0x93fe,0); MCU_Write16(cpu,0xac5a,elapsed);
                MCU_Write16(cpu,0x942e,0x9600); MCU_Write(cpu,0x9607,128);
                MCU_Write(cpu,0x942d,1); MCU_Write16(cpu,0x9446,uint16_t(81000));
                MCU_Write(cpu,0x9429,1); MCU_Write16(cpu,0x943e,uint16_t(81000));
                MCU_Write(cpu,0x94a4,128); MCU_Write16(cpu,0x94a6,0); cpu.r[0] = 0x9400;
                execute(0x5175,0x5367);
                const auto result = glide.advance(elapsed,uint8_t(rateIndex),glideRates,81000,128,pitchConversion);
                const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                if (!result || *result != word(0x9448)
                    || glide.increment != ((uint32_t(MCU_Read(cpu,0x942b))<<16)|word(0x9442))
                    || glide.pitch.accumulator != ((uint32_t(MCU_Read(cpu,0x942d))<<16)|word(0x9446)))
                    throw std::runtime_error("Pitch glide update differs");
                ++glideChecks;
            }
    std::printf("Native pitch glide: %u H8 cases matched\n",glideChecks);
    unsigned pitchDecayChecks = 0;
    for (uint32_t increment : {1u,65535u,65536u,0x7fffffu,0x800000u,0x800001u,0xff0000u,0xffffffu})
        for (uint16_t elapsed : {uint16_t(0),uint16_t(1),uint16_t(2),uint16_t(255),uint16_t(256),uint16_t(32767),uint16_t(32768),uint16_t(65535)})
            for (uint16_t rate : {uint16_t(0),uint16_t(1),uint16_t(2),uint16_t(255),uint16_t(256),uint16_t(32767),uint16_t(32768),uint16_t(65535)})
            {
                sc55::VoicePitch pitch{81000,{128,0}};
                MCU_Write16(cpu,0xac5a,elapsed); MCU_Write16(cpu,0x942e,0x9600); MCU_Write(cpu,0x9607,128);
                MCU_Write(cpu,0x942d,1); MCU_Write16(cpu,0x9446,uint16_t(81000));
                MCU_Write(cpu,0x9429,1); MCU_Write16(cpu,0x943e,uint16_t(81000));
                MCU_Write(cpu,0x94a4,128); MCU_Write16(cpu,0x94a6,0);
                cpu.r[0] = 0x9400; cpu.r[4] = uint16_t(increment>>16); cpu.r[5] = uint16_t(increment); cpu.r[6] = rate;
                execute(0x519b,0x5367);
                const auto decayed = sc55::DecayPitchIncrement(increment,elapsed,rate);
                const auto result = pitch.advance(decayed,81000,128,pitchConversion);
                const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                if (decayed != ((uint32_t(MCU_Read(cpu,0x942b))<<16)|word(0x9442))
                    || pitch.accumulator != ((uint32_t(MCU_Read(cpu,0x942d))<<16)|word(0x9446)) || result != word(0x9448))
                    throw std::runtime_error("Pitch increment decay differs");
                ++pitchDecayChecks;
            }
    std::printf("Native pitch increment decay: %u H8 composed cases matched\n",pitchDecayChecks);
    unsigned pitchAdvanceChecks = 0;
    for (unsigned fixture = 0; fixture < 64; ++fixture)
    {
        sc55::VoicePitch pitch{uint32_t(fixture*262143u),{128,0}};
        MCU_Write16(cpu,0x942e,0x9600);
        MCU_Write(cpu,0x942d,uint8_t(pitch.accumulator>>16)); MCU_Write16(cpu,0x9446,uint16_t(pitch.accumulator));
        MCU_Write(cpu,0x94a4,128); MCU_Write16(cpu,0x94a6,0);
        for (unsigned tick = 0; tick < 256; ++tick)
        {
            const uint32_t increment = (tick%3 == 0 ? 0xffffffu : tick%3 == 1 ? 65537u : 0x800000u);
            const uint32_t reference = (fixture*1237u+tick*17u)&0xffffff;
            const uint8_t source = uint8_t(tick/3);
            MCU_Write(cpu,0x9607,source); MCU_Write(cpu,0x9429,uint8_t(reference>>16)); MCU_Write16(cpu,0x943e,uint16_t(reference));
            cpu.r[0] = 0x9400; cpu.r[4] = uint16_t(increment>>16); cpu.r[5] = uint16_t(increment);
            execute(0x51d5,0x5367);
            const auto result = pitch.advance(increment,reference,source,pitchConversion);
            const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
            if (pitch.accumulator != ((uint32_t(MCU_Read(cpu,0x942d))<<16)|word(0x9446))
                || pitch.correction.source != MCU_Read(cpu,0x94a4) || pitch.correction.offset != word(0x94a6)
                || result != word(0x9448)) throw std::runtime_error("Persistent pitch advance differs");
            ++pitchAdvanceChecks;
        }
    }
    std::printf("Native persistent pitch: %u H8 updates matched\n",pitchAdvanceChecks);
    unsigned pitchCacheChecks = 0;
    for (uint32_t reference : {0u,1u,68999u,69000u,80999u,81000u,92999u,93000u,200000u,0x800000u,0x813c68u,0xffffffu})
        for (unsigned source = 0; source < 256; ++source)
            for (bool hit : {false,true})
            {
                const uint16_t base = uint16_t(source*257);
                sc55::PitchCorrectionCache cache{uint8_t(hit ? source : source+1),uint16_t(source*397)};
                MCU_Write16(cpu,0x942e,0x9600); MCU_Write(cpu,0x9607,uint8_t(source));
                MCU_Write(cpu,0x94a4,cache.source); MCU_Write16(cpu,0x94a6,cache.offset);
                MCU_Write(cpu,0x9429,uint8_t(reference>>16)); MCU_Write16(cpu,0x943e,uint16_t(reference));
                MCU_Write16(cpu,0xc8b0,base); cpu.r[0] = 0x9400;
                execute(0x527c,0x5367);
                const auto result = cache.update(uint8_t(source),reference,base,pitchConversion);
                const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                if (cache.source != MCU_Read(cpu,0x94a4) || cache.offset != word(0x94a6) || result != word(0x9448))
                    throw std::runtime_error("Pitch correction cache differs");
                ++pitchCacheChecks;
            }
    std::printf("Native pitch correction cache: %u H8 cases matched\n",pitchCacheChecks);
    unsigned pitchPreparationChecks = 0;
    for (unsigned source = 0; source < 256; ++source)
        for (unsigned variant = 0; variant < 512; ++variant)
        {
            const auto divisor = uint16_t(variant < 256 ? variant+1 : 65535-variant);
            MCU_Write(cpu,0x94a4,uint8_t(source));
            cpu.r[0] = 0x9400; cpu.r[4] = divisor;
            execute(0x5326,0x534b);
            const auto actual = uint16_t((MCU_Read(cpu,0x94a6)<<8)|MCU_Read(cpu,0x94a7));
            if (sc55::PreparePitchOffset(uint8_t(source),divisor) != actual)
                throw std::runtime_error("Pitch offset preparation differs");
            ++pitchPreparationChecks;
        }
    std::printf("Native pitch offset preparation: %u H8 cases matched\n",pitchPreparationChecks);
    unsigned pitchOffsetChecks = 0;
    for (uint16_t reference : {uint16_t(0),uint16_t(1),uint16_t(16384),uint16_t(32767),uint16_t(32768),uint16_t(65534),uint16_t(65535)})
        for (unsigned offset = 0; offset < 65536; ++offset)
        {
            MCU_Write16(cpu,0xc8b0,reference); MCU_Write16(cpu,0x94a6,uint16_t(offset));
            cpu.r[0] = 0x9400;
            execute(0x534b,0x5367);
            const auto actual = uint16_t((MCU_Read(cpu,0x9448)<<8)|MCU_Read(cpu,0x9449));
            if (actual != sc55::ApplyPitchOffset(reference,uint16_t(offset)))
                throw std::runtime_error("Pitch offset saturation differs");
            ++pitchOffsetChecks;
        }
    std::printf("Native pitch offset: %u H8 cases matched\n",pitchOffsetChecks);
    unsigned groupStopChecks = 0;
    {
        auto hardware = std::make_unique<pcm_t>();
        auto nativeHardware = std::make_unique<pcm_t>();
        struct RestorePcm { mcu_t& cpu; pcm_t* previous; ~RestorePcm() { cpu.pcm = previous; } } restore{cpu,cpu.pcm};
        cpu.pcm = hardware.get();
        unsigned periodicPcmChecks = 0;
        PCM_Init(*hardware,cpu); PCM_Init(*nativeHardware,cpu);
        const auto terminationBr = cpu.br;
        cpu.br = 0xe0;
        unsigned terminationChecks = 0;
        for (uint16_t initialStage : {uint16_t(14),uint16_t(16)})
            for (unsigned value = 0; value < 65560; ++value)
            {
                const auto channel = value%24;
                const auto level = uint16_t(value < 65536 ? value : 0);
                sc55::VoiceLinks links;
                if (value%3 == 0) links.first[channel] = uint8_t((channel+1)%24);
                if (value%3 == 1) links.second[channel] = uint8_t((channel+2)%24);
                for (unsigned slot = 0; slot < 24; ++slot)
                {
                    MCU_Write(cpu,0xcac4+slot,links.first[slot]);
                    MCU_Write(cpu,0xcadc+slot,links.second[slot]);
                }
                hardware->ram2[channel][9] = nativeHardware->ram2[channel][9] = initialStage == 14 ? level : uint16_t(~level);
                hardware->ram2[channel][10] = nativeHardware->ram2[channel][10] = initialStage == 16 ? level : uint16_t(~level);
                MCU_Write16(cpu,0x9400,initialStage); MCU_Write16(cpu,0x93fe,uint16_t(channel));
                MCU_Write(cpu,0xac42+channel,77);
                cpu.r[0] = 0x9400;
                execute(0x3363,0x33d8,0x33e7);
                auto stage = initialStage; uint8_t activity = 77;
                const auto result = sc55::PollEnvelopeTermination(channel,stage,activity,links,
                    [&](uint8_t a) { return PCM_Read(*nativeHardware,a); },
                    [&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); });
                if (!result || (*result == sc55::EnvelopeTermination::notifyAllocator) != (cpu.pc == 0x33d8)
                    || stage != uint16_t(MCU_Read(cpu,0x9400)*256+MCU_Read(cpu,0x9401))
                    || activity != MCU_Read(cpu,0xac42+channel)
                    || std::memcmp(hardware->ram2,nativeHardware->ram2,sizeof(hardware->ram2)) != 0)
                    throw std::runtime_error("Envelope termination poll differs from H8 at "+std::to_string(value));
                for (unsigned slot = 0; slot < 24; ++slot)
                    if (links.first[slot] != MCU_Read(cpu,0xcac4+slot) || links.second[slot] != MCU_Read(cpu,0xcadc+slot))
                        throw std::runtime_error("Envelope termination links differ from H8");
                ++terminationChecks;
            }
        for (unsigned channel = 0; channel < 24; ++channel)
        {
            sc55::VoiceLinks links;
            for (unsigned slot = 0; slot < 24; ++slot)
            { MCU_Write(cpu,0xcac4+slot,255); MCU_Write(cpu,0xcadc+slot,255); }
            hardware->ram2[channel][9] = nativeHardware->ram2[channel][9] = 12345;
            hardware->ram2[channel][10] = nativeHardware->ram2[channel][10] = 54321;
            MCU_Write16(cpu,0x9400,12); MCU_Write16(cpu,0x93fe,uint16_t(channel));
            MCU_Write(cpu,0xac42+channel,99);
            const auto stack = cpu.r[7]; cpu.r[0] = 0x9400; cpu.r[1] = uint16_t(channel);
            execute(0x3472,0x33d8);
            uint16_t stage = 12; uint8_t activity = 99;
            if (!sc55::FinishEnvelopeTermination(channel,stage,activity,links,
                    [&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); })
                || stage != uint16_t(MCU_Read(cpu,0x9400)*256+MCU_Read(cpu,0x9401))
                || activity != MCU_Read(cpu,0xac42+channel) || cpu.r[7] != uint16_t(stack+2)
                || std::memcmp(hardware->ram2,nativeHardware->ram2,sizeof(hardware->ram2)) != 0)
                throw std::runtime_error("Natural envelope termination differs from H8");
            cpu.r[7] = stack;
        }
        cpu.br = terminationBr;
        std::printf("Native envelope termination: %u H8/PCM cases matched\n",terminationChecks);
        std::puts("Native natural termination: 24 direct H8 exits matched with nonzero PCM levels");
        unsigned syncVoiceChecks = 0;
        for (unsigned channel = 0; channel < 24; ++channel)
            for (unsigned flags = 0; flags < 8; ++flags)
                for (unsigned stage : {0u,2u,12u,22u})
                {
                    sc55::VoicePcmLevelState levels{12345,uint16_t(flags&1 ? 0xff00 : 0xb6),45678,uint16_t(flags&4 ? 0xff00 : 0xb6)};
                    sc55::EnvelopeRunner amplitude({},{{stage == 22 ? sc55::EnvelopeStage::finished : sc55::EnvelopeStage(stage/2),{},{},0,0},65535,uint16_t(flags&2 ? 0xff00 : 0xb6),0});
                    sc55::VoiceReleaseAuxiliary release; release.activity = 77;
                    sc55::SecondEnvelopePcmState second{57,9876,levels.level36,levels.command1a};
                    MCU_Write16(cpu,0x9400,uint16_t(stage)); MCU_Write16(cpu,0x93fe,uint16_t(channel));
                    MCU_Write16(cpu,0x9418,levels.level32); MCU_Write16(cpu,0x941a,levels.command16);
                    MCU_Write16(cpu,0x941c,65535); MCU_Write16(cpu,0x941e,amplitude.state().pcmWord);
                    MCU_Write16(cpu,0x9424,levels.level36); MCU_Write16(cpu,0x9426,levels.command1a); MCU_Write(cpu,0xac42+channel,77);
                    // No request for active stages; stopped voices must retain
                    // their request because 3196 branches before 3212.
                    release.pending = stage == 22 ? 1 : 0;
                    MCU_Write(cpu,0xac2a+channel,release.pending);
                    sc55::PitchEnvelopeRunner entryPitch;
                    cpu.r[0] = 0x9400; cpu.br = 0xe0; execute(0x318c,0x32f0,0x3363);
                    const auto entry = sc55::BeginVoiceUpdate(uint8_t(channel),amplitude,release,entryPitch,
                        levels.level32,levels.command16,second,
                        [&](uint8_t a) { return PCM_Read(*nativeHardware,a); },[&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); });
                    if (entry != (stage == 22 ? sc55::VoiceUpdateEntry::stopped : sc55::VoiceUpdateEntry::update)
                        || release.pending != MCU_Read(cpu,0xac2a+channel))
                        throw std::runtime_error("Voice update entry differs");
                    const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                    if (levels.level32 != word(0x9418) || second.level != word(0x9424) || amplitude.state().level != word(0x941c)
                        || second.control != 57 || second.output != 9876 || second.command != levels.command1a
                        || release.activity != MCU_Read(cpu,0xac42+channel)
                        || std::memcmp(hardware->ram1,nativeHardware->ram1,sizeof(hardware->ram1)) != 0
                        || std::memcmp(hardware->ram2,nativeHardware->ram2,sizeof(hardware->ram2)) != 0)
                        throw std::runtime_error("Voice PCM synchronization differs");
                    ++syncVoiceChecks;
                }
        cpu.br = 0;
        std::printf("Native voice PCM synchronization: %u H8/PCM cases matched\n",syncVoiceChecks);
        for (unsigned channel = 0; channel < 24; ++channel)
            for (unsigned stage = 0; stage < 32; ++stage)
            {
                sc55::VoiceStopState voice;
                voice.stages[0] = uint16_t(stage);
                voice.cached18 = uint16_t(0xff00+stage); voice.cached16 = uint16_t(stage*211);
                sc55::VoicePitchRunner pitch;
                pitch.pcmWord = pitchConversion.fromDelta(channel*1000u-12000u);
                sc55::PrepareVoicePitch(voice,pitch);
                sc55::PreparedVoicePcm prepared;
                prepared.pcm12 = uint16_t(channel*521); prepared.pcm14 = uint16_t(stage*1337);
                prepared.pcm1c = uint16_t(channel*stage*79); prepared.pcm1a = 0xdead;
                const uint16_t cached1a = uint16_t(65535-stage*313);
                sc55::SecondEnvelopePcmState second{uint8_t((channel*7+stage)%128),123,456,cached1a};
                MCU_Write16(cpu,0x9400,uint16_t(stage)); MCU_Write16(cpu,0x93fe,uint16_t(channel));
                MCU_Write16(cpu,0x941e,voice.cached18); MCU_Write16(cpu,0x941a,voice.cached16);
                MCU_Write16(cpu,0x9434,prepared.pcm12); MCU_Write16(cpu,0x943a,prepared.pcm14);
                MCU_Write16(cpu,0x9426,cached1a); MCU_Write(cpu,0x9468,second.control); MCU_Write(cpu,0x9466,uint8_t(prepared.pcm1c));
                MCU_Write16(cpu,0x9448,voice.pcm10);
                cpu.r[0] = 0x9400; cpu.br = 0xe0; execute(0x5855,0x5898);
                unsigned writes = 0;
                const auto result = sc55::UpdateVoicePcm(uint8_t(channel),voice,prepared,second,[&](uint8_t a,uint8_t v) {
                    ++writes; PCM_Write(*nativeHardware,a,v);
                });
                const bool active = stage > 0 && stage < 14;
                if (writes != (active ? 15u : 0u) || (result == sc55::VoicePcmUpdateResult::written) != active
                    || std::memcmp(hardware->ram1,nativeHardware->ram1,sizeof(hardware->ram1)) != 0
                    || std::memcmp(hardware->ram2,nativeHardware->ram2,sizeof(hardware->ram2)) != 0)
                    throw std::runtime_error("Periodic voice PCM update differs");
                ++periodicPcmChecks;
            }
        cpu.br = 0;
        std::printf("Native periodic voice PCM update: %u H8/PCM cases matched\n",periodicPcmChecks);
        unsigned partPreparationChecks = 0;
        unsigned composedPitchChecks = 0;
        for (unsigned selector = 0; selector < 40; ++selector)
            for (unsigned flags = 0; flags < 256; ++flags)
            {
                sc55::PreparedPartPitch state{{60000,0,0},0,{{90000,{128,0}},17}};
                PCM_Init(*hardware,cpu); PCM_Init(*nativeHardware,cpu);
                const auto put24 = [&](unsigned hi,unsigned lo,uint32_t v) { MCU_Write(cpu,hi,uint8_t(v>>16)); MCU_Write16(cpu,lo,uint16_t(v)); };
                const auto get24 = [&](unsigned hi,unsigned lo) { return (uint32_t(MCU_Read(cpu,hi))<<16)|(MCU_Read(cpu,lo)<<8)|MCU_Read(cpu,lo+1); };
                put24(0x9428,0x943c,60000); put24(0x942d,0x9446,90000); put24(0x942b,0x9442,17);
                MCU_Write(cpu,0x949a,0); MCU_Write16(cpu,0x94a0,0x9800);
                MCU_Write(cpu,0x9498,0); MCU_Write16(cpu,0x949c,0x9700);
                MCU_Write(cpu,0x9499,0); MCU_Write16(cpu,0x949e,0x9600);
                MCU_Write(cpu,0x9713,uint8_t(selector)); MCU_Write16(cpu,0x93fe,0);
                for (unsigned tick = 0; tick < 4; ++tick)
                {
                    const sc55::PartPitchInputs input{uint8_t(flags+tick*61),uint16_t(flags*257),uint8_t(255-flags),
                        uint16_t(tick*21845),uint16_t(flags*131),uint8_t(selector),uint8_t(flags),uint8_t(tick*85),
                        uint8_t(flags),uint8_t(tick%2 ? 255 : 60)};
                    MCU_Write(cpu,0xc98c,input.partKey); MCU_Write16(cpu,0xc9a4,input.partTune);
                    MCU_Write(cpu,0x980b,input.sampleKey); MCU_Write16(cpu,0x980c,input.sampleTune); MCU_Write16(cpu,0x980e,input.alternateTune);
                    MCU_Write(cpu,0x960b,input.fine); MCU_Write(cpu,0x960c,input.randomDepth);
                    MCU_Write(cpu,0x93c5,input.flags); MCU_Write(cpu,0xc974,input.sourceKey);
                    hardware->ram2[30][10] = nativeHardware->ram2[30][10] = uint16_t(flags*257+tick*8191);
                    cpu.br = 0xe0; cpu.r[0] = 0x9400; cpu.r[1] = 0;
                    execute(0x485c,0x4a47);
                    if (!state.prepare(input,&pitchKeyTables,
                        [&](uint8_t a) { return PCM_Read(*nativeHardware,a); },
                        [&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); })
                        || state.values.pitch != get24(0x9428,0x943c) || state.values.pitch != get24(0x946e,0x9478)
                        || state.values.reference != get24(0x9429,0x943e) || state.values.alternateReference != get24(0x942a,0x9440)
                        || state.cachedRandom != MCU_Read(cpu,0x93c4)
                        || state.glide.increment != get24(0x942b,0x9442) || state.glide.pitch.accumulator != get24(0x942d,0x9446)
                        || hardware->read_latch != nativeHardware->read_latch || hardware->select_channel != nativeHardware->select_channel)
                        throw std::runtime_error("Composed persistent part pitch differs");
                    ++composedPitchChecks;
                }
            }
        std::printf("Native composed persistent part pitch: %u H8 updates matched\n",composedPitchChecks);
        for (unsigned random = 0; random < 256; ++random)
            for (uint32_t initial : {0u,60000u,0x7fffffu,0xffffffu})
                for (uint8_t depth : {uint8_t(0),uint8_t(1),uint8_t(127),uint8_t(255)})
                {
                    PCM_Init(*hardware,cpu); PCM_Init(*nativeHardware,cpu);
                    hardware->ram2[30][10] = nativeHardware->ram2[30][10] = uint16_t(random*257);
                    const auto put24 = [&](unsigned hi,unsigned lo,uint32_t value) { MCU_Write(cpu,hi,uint8_t(value>>16)); MCU_Write16(cpu,lo,uint16_t(value)); };
                    const auto get24 = [&](unsigned hi,unsigned lo) { return (uint32_t(MCU_Read(cpu,hi))<<16)|(MCU_Read(cpu,lo)<<8)|MCU_Read(cpu,lo+1); };
                    put24(0x9428,0x943c,initial); put24(0xc8b2,0xc8ae,60000);
                    put24(0x942d,0x9446,90000); put24(0x942b,0x9442,17);
                    MCU_Write(cpu,0x960b,uint8_t(random)); MCU_Write(cpu,0x960c,depth);
                    MCU_Write(cpu,0x93c5,depth); MCU_Write16(cpu,0x93fe,0); MCU_Write(cpu,0xc974,60);
                    cpu.br = 0xe0; cpu.r[0] = 0x9400; cpu.r[5] = 0x9600;
                    execute(0x4927,0x4a47);
                    const auto prepared = sc55::PreparePartPitch(initial,uint8_t(random),depth,
                        [&](uint8_t a) { return PCM_Read(*nativeHardware,a); },
                        [&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); });
                    sc55::PitchGlide glide{{90000,{128,0}},17};
                    glide.prepare(depth,60,60000,prepared.pitch);
                    if (prepared.pitch != get24(0x9428,0x943c) || prepared.pitch != get24(0x946e,0x9478)
                        || prepared.cachedRandom != MCU_Read(cpu,0x93c4)
                        || glide.increment != get24(0x942b,0x9442) || glide.pitch.accumulator != get24(0x942d,0x9446)
                        || hardware->read_latch != nativeHardware->read_latch
                        || hardware->select_channel != nativeHardware->select_channel)
                        throw std::runtime_error("Part pitch preparation transaction differs");
                    ++partPreparationChecks;
                }
        cpu.br = 0;
        std::printf("Native part pitch preparation with PCM and glide: %u H8 cases matched\n",partPreparationChecks);
        unsigned postEnableChecks = 0;
        for (unsigned channel = 0; channel < 24; ++channel)
            for (unsigned variant = 0; variant < 256; ++variant)
            {
                PCM_Init(*hardware,cpu); PCM_Init(*nativeHardware,cpu);
                sc55::VoicePostEnable state{uint8_t(variant%3 == 0 ? variant : 0),
                    uint16_t(variant*257),uint16_t(variant*397)};
                const uint16_t status = uint16_t(variant*131);
                hardware->ram2[channel][7] = nativeHardware->ram2[channel][7] = status;
                MCU_Write16(cpu,0x93fe,uint16_t(channel)); MCU_Write(cpu,0x9465,state.field65);
                MCU_Write16(cpu,0x93ea,state.level); MCU_Write16(cpu,0x9426,state.command);
                cpu.cp = cpu.dp = cpu.ep = 0; cpu.br = 0xe0; cpu.pc = 0x580a; cpu.r[0] = 0x9400;
                unsigned instructions = 0;
                bool polled = false;
                while (cpu.pc != 0x582d)
                {
                    if (cpu.pc == 0x5814 && polled) break;
                    if (cpu.pc == 0x5814) polled = true;
                    if (++instructions > 32) throw std::runtime_error("Post-enable loop escaped");
                    const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                }
                const auto result = sc55::PollVoicePostEnable(channel,state,
                    [&](uint8_t a) { return PCM_Read(*nativeHardware,a); },
                    [&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); });
                if (!result || *result != (cpu.pc == 0x582d)
                    || std::memcmp(hardware->ram2,nativeHardware->ram2,sizeof(hardware->ram2)) != 0
                    || hardware->read_latch != nativeHardware->read_latch
                    || hardware->write_latch != nativeHardware->write_latch
                    || hardware->select_channel != nativeHardware->select_channel)
                    throw std::runtime_error("Post-enable PCM differs");
                ++postEnableChecks;
            }
        cpu.br = 0;
        std::printf("Native post-enable poll: %u H8/PCM cases matched\n",postEnableChecks);
        unsigned keyMaskChecks = 0;
        for (unsigned channel = 0; channel < 24; ++channel)
            for (unsigned variant = 0; variant < 256; ++variant)
            {
                PCM_Init(*hardware,cpu); PCM_Init(*nativeHardware,cpu);
                hardware->voice_mask = nativeHardware->voice_mask = 0x0abcdef0;
                hardware->voice_mask_pending = nativeHardware->voice_mask_pending = 0x01234567;
                hardware->voice_mask_updating = nativeHardware->voice_mask_updating = true;
                sc55::VoiceKeyMask mask{uint32_t(0xffffffffu-variant*0x01010101u),
                    uint32_t((1u<<channel) | (variant*0x0013579bu))};
                sc55::VoiceStopState state;
                state.fieldCAF4 = variant%4 == 0 ? 0 : uint8_t(variant);
                MCU_Write16(cpu,0x93fe,uint16_t(channel)); MCU_Write(cpu,0xcaf4+channel,state.fieldCAF4);
                MCU_Write16(cpu,0xcb24,uint16_t(mask.enabled>>16)); MCU_Write16(cpu,0xcb26,uint16_t(mask.enabled));
                MCU_Write16(cpu,0xcb28,uint16_t(mask.prepared>>16)); MCU_Write16(cpu,0xcb2a,uint16_t(mask.prepared));
                cpu.r[0] = 0x9400; cpu.br = 0xe0;
                execute(0x573f,state.fieldCAF4 ? 0x56d9 : 0x576f);
                const bool result = sc55::RemovePreparedVoiceKeys(mask,state,
                    [&](uint8_t a) { return PCM_Read(*nativeHardware,a); },
                    [&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); });
                uint32_t actual = 0;
                for (unsigned i = 0; i < 4; ++i) actual = (actual<<8)|MCU_Read(cpu,0xcb24+i);
                if (result != (state.fieldCAF4 == 0) || actual != mask.enabled
                    || hardware->voice_mask != nativeHardware->voice_mask
                    || hardware->voice_mask_pending != nativeHardware->voice_mask_pending
                    || hardware->voice_mask_updating != nativeHardware->voice_mask_updating)
                    throw std::runtime_error("Prepared key-mask update differs");
                execute(0x582e,0x5854);
                sc55::EnablePreparedVoiceKeys(mask,
                    [&](uint8_t a) { return PCM_Read(*nativeHardware,a); },
                    [&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); });
                actual = 0;
                for (unsigned i = 0; i < 4; ++i)
                {
                    actual = (actual<<8)|MCU_Read(cpu,0xcb24+i);
                    if (MCU_Read(cpu,0xcb28+i) != 0) throw std::runtime_error("Prepared key batch not cleared");
                }
                if (actual != mask.enabled || mask.prepared != 0
                    || hardware->voice_mask != nativeHardware->voice_mask
                    || hardware->voice_mask_pending != nativeHardware->voice_mask_pending
                    || hardware->voice_mask_updating != nativeHardware->voice_mask_updating)
                    throw std::runtime_error("Prepared key-mask enable differs");
                ++keyMaskChecks;
            }
        cpu.br = 0;
        std::printf("Native prepared key mask: %u H8/PCM cases matched\n",keyMaskChecks);
        unsigned preparedActivationChecks = 0;
        for (unsigned channel = 0; channel < 24; ++channel)
            for (unsigned variant = 0; variant < 256; ++variant)
            {
                PCM_Init(*hardware,cpu); PCM_Init(*nativeHardware,cpu);
                PCM_Write(*hardware,0x3e,uint8_t(channel)); PCM_Write(*nativeHardware,0x3e,uint8_t(channel));
                sc55::VoiceStopState state;
                state.stages = {0x12,0x14,0x16}; state.cached18 = uint16_t(variant*257);
                state.savedStage = uint16_t(variant); state.progress = uint16_t(65535-variant);
                state.delayAccumulator = variant%3 == 0 ? 0 : uint16_t(variant+1);
                state.pcm10 = uint16_t(variant*131); state.flagMinus3B = uint8_t(variant);
                sc55::VoicePitchRunner preparedPitch;
                preparedPitch.envelope.stage = state.stages[2];
                preparedPitch.envelope.segment.progress = {123,17};
                preparedPitch.glide.increment = 456;
                preparedPitch.pcmWord = pitchConversion.fromDelta(uint32_t(variant*127)-12000u);
                sc55::PrepareVoicePitch(state,preparedPitch);
                const std::array<unsigned,8> offsets{0,2,4,6,8,14,30,72};
                const auto fields = [&]() { return std::array<uint16_t,8>{state.stages[0],state.stages[1],state.stages[2],state.savedStage,
                    state.progress,state.delayAccumulator,state.cached18,state.pcm10}; };
                const auto initialFields = fields();
                for (unsigned i = 0; i < offsets.size(); ++i) MCU_Write16(cpu,0x9400+offsets[i],initialFields[i]);
                MCU_Write(cpu,0x93c5,state.flagMinus3B);
                sc55::PreparedVoicePcm prepared;
                prepared.sample.mode = uint16_t(variant*73+channel);
                prepared.pcm12 = uint16_t(variant*311); prepared.pcm14 = uint16_t(variant*197);
                prepared.pcm1c = uint16_t(variant*173); prepared.pcm1a = uint16_t(variant*503);
                sc55::SecondEnvelopeReleaseState preparedSecond;
                preparedSecond.stage = state.stages[1];
                preparedSecond.progress = {321,19}; preparedSecond.level = 12345;
                sc55::SecondEnvelopeSetup secondSetup;
                secondSetup.mode.pcmMode = uint8_t(prepared.pcm1c);
                secondSetup.activation = {uint16_t(variant*149),prepared.pcm1a};
                sc55::SecondEnvelopePcmState secondPcm;
                secondPcm.control = uint8_t(prepared.pcm1c>>8);
                secondPcm.command = uint16_t(variant*271);
                sc55::VoicePostEnable secondPost{7,0,0};
                sc55::PrepareVoiceSecondEnvelope(state,prepared,secondPost,preparedSecond,secondSetup,secondPcm);
                if (secondPost.field65 != 7 || secondPost.level != uint16_t(variant*149)
                    || secondPost.command != uint16_t(variant*271))
                    throw std::runtime_error("Second envelope post-enable mapping differs");
                state.cached16 = uint16_t(variant*227);
                state.fieldC8B3 = state.fieldCB30 = 255;
                MCU_Write16(cpu,0x93fe,uint16_t(channel));
                MCU_Write(cpu,0xc8b3+channel,255); MCU_Write(cpu,0xcb30+channel,255);
                MCU_Write16(cpu,0x93f6,prepared.sample.mode);
                const std::array<unsigned,3> highOffsets{0x93ec,0x93ee,0x93ed};
                const std::array<unsigned,3> lowOffsets{0x93f0,0x93f4,0x93f2};
                std::array<uint32_t*,3> addresses{&prepared.sample.start,&prepared.sample.loop,&prepared.sample.end};
                for (unsigned i = 0; i < 3; ++i)
                {
                    const auto high = uint8_t(variant+71*i);
                    const auto low = uint16_t(variant*397+12345*i);
                    *addresses[i] = (uint32_t(high)<<16)|low;
                    MCU_Write(cpu,highOffsets[i],high);
                    MCU_Write16(cpu,lowOffsets[i],low);
                }
                MCU_Write16(cpu,0x9434,prepared.pcm12); MCU_Write16(cpu,0x943a,prepared.pcm14);
                MCU_Write(cpu,0x9468,uint8_t(prepared.pcm1c>>8)); MCU_Write(cpu,0x9466,uint8_t(prepared.pcm1c));
                MCU_Write16(cpu,0x93e8,prepared.pcm1a); MCU_Write16(cpu,0x941a,state.cached16);
                cpu.r[0] = 0x9400; cpu.br = 0xe0;
                execute(0x5777,variant & 128 ? 0x57f5 : 0x5809);
                if (!sc55::CommitPreparedVoice(channel,state,prepared,[&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); }))
                    throw std::runtime_error("Prepared commit rejected valid channel");
                if (state.fieldC8B3 != MCU_Read(cpu,0xc8b3+channel) || state.fieldCB30 != MCU_Read(cpu,0xcb30+channel))
                    throw std::runtime_error("Prepared commit flags differ");
                const auto expectedFields = fields();
                const auto continuedSecond = sc55::ContinueVoiceSecondEnvelope(state,preparedSecond);
                const auto secondStage = uint16_t((MCU_Read(cpu,0x9402)<<8)|MCU_Read(cpu,0x9403));
                if (!continuedSecond || continuedSecond->stage != secondStage
                    || continuedSecond->level != 12345 || continuedSecond->progress.position != 321
                    || continuedSecond->progress.deferredTicks != 19)
                    throw std::runtime_error("Second envelope commit handoff differs");
                const auto continuedPitch = sc55::ContinueVoicePitch(state,preparedPitch);
                const auto pitchStage = uint16_t((MCU_Read(cpu,0x9404)<<8)|MCU_Read(cpu,0x9405));
                if (pitchStage <= 22 && (pitchStage&1) == 0)
                {
                    if (!continuedPitch || continuedPitch->envelope.stage != pitchStage
                        || continuedPitch->pcmWord != state.pcm10 || continuedPitch->glide.increment != 456
                        || continuedPitch->envelope.segment.progress.position != 123 || continuedPitch->envelope.segment.progress.deferredTicks != 17)
                        throw std::runtime_error("Pitch commit handoff differs");
                }
                else if (continuedPitch) throw std::runtime_error("Pitch commit accepted invalid stage");
                for (unsigned i = 0; i < offsets.size(); ++i)
                {
                    const unsigned a = 0x9400+offsets[i];
                    if (((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)) != expectedFields[i])
                        throw std::runtime_error("Prepared activation state differs");
                }
                if (std::memcmp(hardware->ram2,nativeHardware->ram2,sizeof(hardware->ram2)) != 0
                    || std::memcmp(hardware->ram1,nativeHardware->ram1,sizeof(hardware->ram1)) != 0)
                    throw std::runtime_error("Prepared activation PCM differs");
                ++preparedActivationChecks;
            }
        cpu.br = 0;
        std::printf("Native prepared commit: %u H8/PCM cases matched\n",preparedActivationChecks);
        unsigned readinessChecks = 0;
        for (unsigned channel = 0; channel < 24; ++channel)
            for (uint8_t flag : {uint8_t(0),uint8_t(1),uint8_t(4),uint8_t(255)})
                for (uint16_t first : {uint16_t(0),uint16_t(1),uint16_t(32767),uint16_t(32768),uint16_t(65535)})
                    for (uint16_t second : {uint16_t(0),uint16_t(1),uint16_t(32767),uint16_t(32768),uint16_t(65535)})
                    {
                        PCM_Init(*hardware,cpu); PCM_Init(*nativeHardware,cpu);
                        hardware->ram2[channel][9] = nativeHardware->ram2[channel][9] = first;
                        hardware->ram2[channel][10] = nativeHardware->ram2[channel][10] = second;
                        MCU_Write16(cpu,0x93fe,uint16_t(channel)); MCU_Write(cpu,0xcaf4+channel,flag);
                        cpu.cp = cpu.dp = cpu.ep = 0; cpu.br = 0xe0; cpu.pc = 0x5710; cpu.r[1] = 0x9400;
                        unsigned instructions = 0;
                        while (cpu.pc != 0x573a && cpu.pc != 0x573e && cpu.pc != 0x56d9)
                        {
                            if (++instructions > 30) throw std::runtime_error("Reuse readiness escaped");
                            const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                        }
                        const auto result = sc55::PollVoiceReuse(uint8_t(channel),flag,
                            [&](uint8_t a) { return PCM_Read(*nativeHardware,a); },
                            [&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); });
                        const auto expected = cpu.pc == 0x573a ? sc55::VoiceReuseReadiness::pending
                            : cpu.pc == 0x573e ? sc55::VoiceReuseReadiness::ready : sc55::VoiceReuseReadiness::cancelled;
                        if (!result || *result != expected) throw std::runtime_error("Reuse readiness differs");
                        ++readinessChecks;
                    }
        cpu.br = 0;
        std::printf("Native reuse readiness: %u H8/PCM cases matched\n",readinessChecks);
        for (unsigned part = 0; part < 16; ++part)
            for (unsigned count : {1u,2u})
                for (unsigned operation : {0u,1u,2u,3u,4u,5u,6u})
                    for (unsigned seed = 0; seed < 8; ++seed)
                    {
                        const bool prepend = operation == 1, choke = operation == 2, repeated = operation >= 3;
                        const uint8_t selector = seed%4 == 0 ? 0 : uint8_t(59+seed%4);
                        PCM_Init(*hardware,cpu); PCM_Init(*nativeHardware,cpu);
                        sc55::VoiceAllocator state;
                        if (!state.initializeTables()) throw std::runtime_error("Invalid test initialization");
                        const auto allocated = state.createGroup({uint8_t(part),60,100,0,uint8_t(count)});
                        if (!allocated) throw std::runtime_error("Invalid test allocation");
                        if (choke || repeated)
                        {
                            for (unsigned i = 0; i < 5; ++i)
                            {
                                const auto extra = state.createGroup({uint8_t(i == 4 ? (part+1)%16 : part),
                                    uint8_t(60+i%2),uint8_t(repeated ? (i == 0 ? 99 : 100) : 40+i),0,uint8_t(count)});
                                if (!extra) throw std::runtime_error("Choke fixture allocation failed");
                                state.groupStatus[extra->group] = uint8_t(i%3);
                                if (repeated && operation != 4 && i == 2) state.groupFieldA288[extra->group] = 4;
                            }
                            state.partFlags[part] = uint8_t(seed); // Hold never inhibits choke.
                        }
                        state.shortage = uint8_t(seed);
                        write(state);
                        std::array<sc55::VoiceStopState,24> stopped;
                        const auto address = [&](unsigned voice) { return uint16_t((cpu.rom1[0x676a+voice*2]<<8)|cpu.rom1[0x676b+voice*2]); };
                        for (unsigned voice = 0; voice < 24; ++voice)
                        {
                            stopped[voice] = {{{2,4,6}},0x1234,0x5678,7,9};
                            const auto base = address(voice);
                            for (unsigned i = 0; i < 3; ++i) MCU_Write16(cpu,base+i*2,stopped[voice].stages[i]);
                            MCU_Write16(cpu,base+0x1a,0x1234); MCU_Write16(cpu,base+0x1e,0x5678);
                            MCU_Write(cpu,0xcb30+voice,7); MCU_Write(cpu,0xcaf4+voice,9);
                            hardware->ram2[voice][3] = nativeHardware->ram2[voice][3] = 0x1234;
                            hardware->ram2[voice][4] = nativeHardware->ram2[voice][4] = 0x5678;
                            hardware->ram2[voice][9] = nativeHardware->ram2[voice][9] = uint16_t(seed*8191);
                            hardware->ram2[voice][10] = nativeHardware->ram2[voice][10] = uint16_t(seed%2 ? seed*8191 : voice*2731);
                        }
                        for (auto& r : cpu.r) r = 0;
                        cpu.r[2] = allocated->group; cpu.r[3] = uint16_t(part); cpu.r[7] = 0x9300;
                        if (repeated)
                        {
                            std::array<uint8_t,16> retained; retained.fill(255);
                            if (operation == 5) retained[0] = 100;
                            if (operation == 6) retained[1] = 100; // after sentinel: not retained
                            const uint8_t flags = uint8_t((seed >= 4 ? 128 : 0) | (seed%4));
                            const uint8_t noteSelector = seed == 4 ? 62 : 60; // mode0 ignores selector
                            MCU_Write16(cpu,0xa1c0,0x9000); MCU_Write(cpu,0x9005,flags);
                            MCU_Write(cpu,0xa3d0,uint8_t(part)); MCU_Write(cpu,0xa3d1,noteSelector); MCU_Write(cpu,0xa3d2,100);
                            for (unsigned i = 0; i < 16; ++i) MCU_Write(cpu,0xa090+part*16+i,retained[i]);
                            execute(0x17b8,0x185c);
                            const auto retired = sc55::RetireRepeatedNote(state,stopped,part,100,noteSelector,flags,retained,
                                [&](uint8_t a) { return PCM_Read(*nativeHardware,a); },
                                [&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); });
                            const bool expected = seed == 4 || (seed == 5 && operation != 4 && operation != 5);
                            if (!retired || *retired != expected || cpu.r[7] != 0x9300)
                                throw std::runtime_error("Repeated note retirement result differs");
                        }
                        else if (choke)
                        {
                            MCU_Write(cpu,0xa3d0,uint8_t(part)); MCU_Write(cpu,0xa3d1,selector);
                            execute(0x0ca5,0x0cb3);
                            const auto stoppedGroups = sc55::StopRhythmExclusiveGroups(state,stopped,part,selector,
                                [&](uint8_t a) { return PCM_Read(*nativeHardware,a); },
                                [&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); });
                            const unsigned expected = selector == 60 ? 3 : selector == 61 ? 2 : 0;
                            if (!stoppedGroups || *stoppedGroups != expected || cpu.r[7] != 0x9300)
                                throw std::runtime_error("Rhythm choke result differs");
                        }
                        else
                        {
                            execute(prepend ? 0x1a53 : 0x1a3b,prepend ? 0x1a68 : 0x1a52);
                            const auto next = sc55::StopAndReclaimGroup(state,stopped,allocated->group,part,prepend,
                                [&](uint8_t a) { return PCM_Read(*nativeHardware,a); },
                                [&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); });
                            if (!next || *next != uint8_t(cpu.r[5]) || cpu.r[7] != 0x9300)
                                throw std::runtime_error("Group stop result differs");
                        }
                        compare(state);
                        const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
                        for (unsigned voice = 0; voice < 24; ++voice)
                        {
                            const auto base = address(voice);
                            for (unsigned i = 0; i < 3; ++i)
                                if (word(base+i*2) != stopped[voice].stages[i]) throw std::runtime_error("Group stop stage differs");
                            if (word(base+0x1a) != stopped[voice].cached16 || word(base+0x1e) != stopped[voice].cached18
                                || MCU_Read(cpu,0xcb30+voice) != stopped[voice].fieldCB30
                                || MCU_Read(cpu,0xcaf4+voice) != stopped[voice].fieldCAF4)
                                throw std::runtime_error("Group stop metadata differs");
                        }
                        if (std::memcmp(hardware->ram2,nativeHardware->ram2,sizeof(hardware->ram2)) != 0)
                            throw std::runtime_error("Group stop PCM registers differ");
                        ++groupStopChecks;
                    }
        unsigned capacityChecks = 0, admittedChecks = 0, rejectedChecks = 0;
        for (bool admission : {false,true})
        for (unsigned seed = 0; seed < 192; ++seed)
        {
            PCM_Init(*hardware,cpu); PCM_Init(*nativeHardware,cpu);
            sc55::VoiceAllocator state;
            state.initializeTables();
            for (unsigned i = 0; i < 12; ++i)
            {
                const auto created = state.createGroup({uint8_t(i%4),60,uint8_t(60+i),0,2});
                if (!created) throw std::runtime_error("Capacity fixture allocation failed");
                for (unsigned j = 0; j < 2; ++j) state.activity[created->voices[j]] = uint8_t(10+i);
                state.groupStatus[created->group] = uint8_t((i+seed)%2);
            }
            sc55::VoiceCapacityPolicy policy;
            policy.startPartControl = uint8_t(seed%16);
            policy.forceOldest = (seed & 16) != 0;
            for (unsigned p = 0; p < 16; ++p)
            {
                policy.reserves[p] = uint8_t(seed & 32 ? 24 : (p+seed)%7);
                policy.modes[p] = uint8_t((seed+p)%3);
                MCU_Write(cpu,0x8018+p,policy.reserves[p]); MCU_Write(cpu,0xa040+p,policy.modes[p]);
            }
            MCU_Write(cpu,0x8028,policy.startPartControl);
            const uint8_t partFlags = uint8_t((policy.forceOldest ? 16 : 0) | (admission ? 128 | (seed%4) : 0));
            MCU_Write16(cpu,0xa1c0,0x9000); MCU_Write(cpu,0x9005,partFlags);
            const unsigned incoming = seed%4, requested = 1+seed%2;
            MCU_Write(cpu,0xa3d0,uint8_t(incoming)); MCU_Write(cpu,0xa3d4,uint8_t(requested));
            write(state);
            std::array<sc55::VoiceStopState,24> stopped{};
            for (unsigned voice = 0; voice < 24; ++voice)
            {
                const auto base = uint16_t((cpu.rom1[0x676a+voice*2]<<8)|cpu.rom1[0x676b+voice*2]);
                for (unsigned i = 0; i < 3; ++i) MCU_Write16(cpu,base+i*2,0);
                MCU_Write16(cpu,base+0x1a,0); MCU_Write16(cpu,base+0x1e,0);
                MCU_Write(cpu,0xcb30+voice,0); MCU_Write(cpu,0xcaf4+voice,0);
                hardware->ram2[voice][9] = nativeHardware->ram2[voice][9] = 100;
                hardware->ram2[voice][10] = nativeHardware->ram2[voice][10] = uint16_t(voice*100);
            }
            for (auto& r : cpu.r) r = 0;
            cpu.r[7] = 0x9300;
            if (admission)
            {
                const sc55::VoiceAllocator::GroupRequest request{uint8_t(incoming),uint8_t(seed%3 == 0 ? 60 : 0),
                    uint8_t(60+seed%12),uint8_t(seed),uint8_t(requested)};
                std::array<uint8_t,16> retained; retained.fill(255);
                if (seed&64) retained[0] = request.value;
                for (unsigned i = 0; i < 16; ++i) MCU_Write(cpu,0xa090+incoming*16+i,retained[i]);
                MCU_Write(cpu,0xa3d1,request.fieldA3D1); MCU_Write(cpu,0xa3d2,request.value);
                MCU_Write(cpu,0xa1bf,request.fieldA1BF);
                execute(0x0ca5,0x0cb6);
                sc55::RhythmGroupAdmission pending(request,partFlags,retained,policy);
                const auto result = pending.run(state,stopped,
                    [&](uint8_t a) { return PCM_Read(*nativeHardware,a); },
                    [&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); });
                using Status = sc55::RhythmGroupAdmission::Status;
                if (result != (uint8_t(cpu.r[0]) == 0 ? Status::allocated : Status::capacityRejected)
                    || cpu.r[7] != 0x9300) throw std::runtime_error("Rhythm admission result differs");
                if (pending.group())
                {
                    ++admittedChecks;
                    if (pending.group()->group != MCU_Read(cpu,0xa3d5)) throw std::runtime_error("Rhythm admission group differs");
                    for (unsigned i = 0; i <= requested; ++i)
                        if (pending.group()->voices[i] != MCU_Read(cpu,0xa3d6+i)) throw std::runtime_error("Rhythm admission slots differ");
                }
                else ++rejectedChecks;
                const auto saved = state;
                if (pending.run(state,stopped,[](uint8_t)->uint8_t { throw std::runtime_error("Admission replay read"); },
                    [](uint8_t,uint8_t) { throw std::runtime_error("Admission replay write"); }) != result
                    || std::memcmp(&saved,&state,sizeof state) != 0) throw std::runtime_error("Admission replay changed state");
            }
            else
            {
            execute(0x1737,0x178c);
            const bool firmwareSuccess = (cpu.sr & STATUS_Z) || (((cpu.sr & STATUS_N) != 0) != ((cpu.sr & STATUS_V) != 0));
            const auto result = sc55::EnsureVoiceCapacity(state,stopped,incoming,requested,policy,
                [&](uint8_t a) { return PCM_Read(*nativeHardware,a); },
                [&](uint8_t a,uint8_t v) { PCM_Write(*nativeHardware,a,v); });
            if (!result || *result != firmwareSuccess || cpu.r[7] != 0x9300)
                throw std::runtime_error("Capacity result differs for seed " + std::to_string(seed));
            }
            compare(state);
            if (std::memcmp(hardware->ram2,nativeHardware->ram2,sizeof(hardware->ram2)) != 0)
                throw std::runtime_error("Capacity PCM registers differ");
            ++capacityChecks;
        }
        std::printf("Native capacity policy: %u H8/PCM cases matched\n",capacityChecks);
        if (admittedChecks == 0 || rejectedChecks == 0) throw std::runtime_error("Rhythm admission coverage missing");
        std::printf("Native rhythm admission: %u allocated, %u capacity-rejected H8 cases matched\n",admittedChecks,rejectedChecks);
    }
    std::printf("Native complete group stop: %u H8/PCM cases matched\n",groupStopChecks);
    // Stop stages 12/14 bypass the normal envelope and level-poll paths.
    // In particular do not model them as 0e/10 (which read PCM32/34).
    unsigned stoppedStageChecks = 0;
    for (uint16_t stage : {uint16_t(0x12),uint16_t(0x14)})
        for (unsigned voice = 0; voice < 24; ++voice)
            for (unsigned seed = 0; seed < 16; ++seed)
            {
                std::array<uint8_t,256> original;
                for (unsigned i = 0; i < original.size(); ++i)
                { original[i] = uint8_t(i*13+seed*17); MCU_Write(cpu,0x9400+i,original[i]); }
                MCU_Write16(cpu,0x9400,stage); original[0] = uint8_t(stage>>8); original[1] = uint8_t(stage);
                MCU_Write16(cpu,0x93fe,uint16_t(voice));
                MCU_Write(cpu,0xac42+voice,uint8_t(seed+1));
                cpu.r[0] = 0x9400;
                execute(0x3363,0x33e7);
                for (unsigned i = 0; i < original.size(); ++i)
                    if (MCU_Read(cpu,0x9400+i) != original[i]) throw std::runtime_error("Stopped stage modified voice state");
                if (MCU_Read(cpu,0xac42+voice) != seed+1) throw std::runtime_error("Stopped stage modified activity");
                ++stoppedStageChecks;
            }
    std::printf("Stopped stage bypass: %u H8 cases verified\n",stoppedStageChecks);
    unsigned stopChecks = 0;
    for (unsigned value = 0; value < 65536; ++value)
        for (uint16_t second : {uint16_t(0),uint16_t(65535),uint16_t(value),uint16_t(value-1),uint16_t(value+1)})
        {
            cpu.cp = cpu.dp = cpu.ep = 0; cpu.pc = 0x5404; cpu.sr = 0;
            cpu.r[4] = uint16_t(value); cpu.r[5] = second;
            unsigned instructions = 0;
            while (cpu.pc != 0x540b && cpu.pc != 0x541b)
            {
                if (++instructions > 5) throw std::runtime_error("Voice stop decision escaped");
                const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
            }
            const auto plan = sc55::PrepareVoiceStop(uint16_t(value),second);
            if (cpu.r[6] != plan.stageCode || plan.pcmAddress != (cpu.pc == 0x540b ? 0x16 : 0x18)
                || plan.cachedWordOffset != (cpu.pc == 0x540b ? 0x1a : 0x1e))
                throw std::runtime_error("Voice stop decision differs");
            ++stopChecks;
        }
    std::printf("Native voice stop decisions: %u cases matched\n",stopChecks);
    unsigned completeStopChecks = 0;
    auto stopPcm = std::make_unique<pcm_t>();
    PCM_Init(*stopPcm,cpu); PCM_UseSimulation(*stopPcm,false);
    auto* previousPcm = cpu.pcm;
    cpu.pcm = stopPcm.get();
    std::array<uint16_t,8> savedStopRegisters;
    std::copy(std::begin(cpu.r),std::end(cpu.r),savedStopRegisters.begin());
    for (unsigned slot = 0; slot < 24; ++slot)
        for (unsigned seed = 0; seed < 16; ++seed)
        {
            const auto word = [&](unsigned a) { return uint16_t((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,a+1)); };
            const auto base = word(0x676a+slot*2);
            sc55::VoiceStopState state;
            state.stages = {2,4,6}; state.cached16 = 0x1234; state.cached18 = 0x5678;
            state.fieldCB30 = 77; state.fieldCAF4 = 99;
            const auto resetPcm = [&] {
                cpu.pcm->ram2[slot][3] = 0x1234; cpu.pcm->ram2[slot][4] = 0x5678;
                cpu.pcm->ram2[slot][9] = uint16_t(seed*4096);
                cpu.pcm->ram2[slot][10] = uint16_t((15-seed)*4096);
            };
            resetPcm();
            if (!sc55::StopPreparedVoice(slot,state,[&](uint8_t a) { return PCM_Read(*cpu.pcm,a); },
                [&](uint8_t a,uint8_t v) { PCM_Write(*cpu.pcm,a,v); })) throw std::runtime_error("Invalid complete stop fixture");
            const auto first = cpu.pcm->ram2[slot][3], second = cpu.pcm->ram2[slot][4];
            resetPcm();
            for (unsigned i = 0; i < 3; ++i) MCU_Write16(cpu,base+i*2,uint16_t(2+i*2));
            MCU_Write16(cpu,base+0x1a,0x1234); MCU_Write16(cpu,base+0x1e,0x5678);
            MCU_Write(cpu,0xcb30+slot,77); MCU_Write(cpu,0xcaf4+slot,99);
            for (auto& r : cpu.r) r = 0;
            cpu.r[1] = uint16_t(slot);
            execute(0x53e6,0x542f);
            for (unsigned i = 0; i < 3; ++i)
                if (word(base+i*2) != state.stages[i]) throw std::runtime_error("Complete stop stage mismatch");
            if (word(base+0x1a) != state.cached16 || word(base+0x1e) != state.cached18
                || MCU_Read(cpu,0xcb30+slot) != state.fieldCB30 || MCU_Read(cpu,0xcaf4+slot) != state.fieldCAF4
                || cpu.pcm->ram2[slot][3] != first || cpu.pcm->ram2[slot][4] != second)
                throw std::runtime_error("Complete stop state/device mismatch");
            ++completeStopChecks;
        }
    std::printf("Native complete voice stop: %u H8 state/device cases matched\n",completeStopChecks);
    std::copy(savedStopRegisters.begin(),savedStopRegisters.end(),std::begin(cpu.r));
    cpu.pcm = previousPcm;
    unsigned candidateChecks = 0;
    for (unsigned pass = 0; pass < 3; ++pass)
        for (unsigned seed = 0; seed < 4096; ++seed)
        {
            sc55::VoiceAllocator state;
            std::array<uint8_t,24> activity;
            uint32_t random = seed+1;
            const auto byte = [&]() { random = random*1664525u+1013904223u; return uint8_t(random>>24); };
            const unsigned part = seed%16, length = 1+seed%24;
            const uint8_t value = uint8_t(seed%8);
            state.partHead[part] = 0;
            for (unsigned i = 0; i < 24; ++i)
            {
                state.groupNext[i] = i+1 < length ? uint8_t(i+1) : 255;
                state.groupValue[i] = byte()%8; state.groupStatus[i] = byte()%2;
                state.groups.tail[i] = byte()%24;
                state.groups.previous[i] = byte()%2 ? uint8_t(byte()%24) : 255;
                activity[i] = seed%3 ? byte() : 255;
                state.activity[i] = activity[i];
                MCU_Write(cpu,0xac42+i,activity[i]);
            }
            write(state); cpu.r[0] = cpu.r[1] = cpu.r[2] = 0;
            cpu.r[3] = uint16_t(part); cpu.r[4] = value;
            const std::array<uint16_t,3> start{0x1961,0x19af,0x19f5}, end{0x19ae,0x19f4,0x1a3a};
            execute(start[pass],end[pass]);
            const auto candidate = state.selectCandidate(part,value,sc55::VoiceAllocator::CandidatePass(pass),activity);
            if (!candidate || candidate->voice != uint8_t(cpu.r[1]) || candidate->activity != uint8_t(cpu.r[0])
                || candidate->voice != MCU_Read(cpu,0xa3cc)) throw std::runtime_error("Voice candidate differs");
            compare(state); ++candidateChecks;
        }
    std::printf("Native voice candidates: %u cases matched\n",candidateChecks);
    unsigned initializationChecks = 0;
    for (unsigned voices = 1; voices <= 24; ++voices)
        for (unsigned groups = 1; groups <= 24; ++groups)
            for (unsigned seed = 0; seed < 4; ++seed)
            {
                sc55::VoiceAllocator state;
                visitAllocatorBytes(state,[&](unsigned address,uint8_t& value) { value = uint8_t(address*13+seed*71); });
                write(state);
                MCU_Write16(cpu,0xa3ca,uint16_t(voices-1));
                cpu.r[2] = uint16_t(groups-1); cpu.r[3] = 15;
                cpu.cp = 4; cpu.dp = cpu.ep = 0; cpu.pc = 0x04b9; cpu.sr = 0;
                unsigned instructions = 0;
                while (cpu.pc != 0x0569)
                {
                    if (++instructions > 1500) throw std::runtime_error("Allocator initialization escaped");
                    const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                }
                if (!state.initializeTables(voices,groups)) throw std::runtime_error("Allocator initialization rejected valid counts");
                compare(state); ++initializationChecks;
            }
    std::printf("Native allocator initialization: %u cases matched\n",initializationChecks);
    unsigned createChecks = 0;
    for (unsigned group = 0; group < 24; ++group)
        for (unsigned part = 0; part < 16; ++part)
            for (unsigned variant = 0; variant < 32; ++variant)
            {
                sc55::VoiceAllocator state;
                state.freeGroupHead = uint8_t(group); state.groupNext[group] = uint8_t((group+1)%24);
                state.partTail[part] = variant & 1 ? uint8_t((group+2)%24) : 255;
                state.partMinimum[part] = variant & 2 ? 255 : uint8_t(variant*7);
                state.freeHead = uint8_t(group); state.freeTail = uint8_t((group+1)%24);
                state.freeNext[group] = state.freeTail; state.freeNext[state.freeTail] = 255;
                state.freeCount = 2; state.partVoiceCount[part] = uint8_t(variant*11);
                state.groupStatus[group] = 0x94; state.groupFieldA288[group] = 0x57;
                const sc55::VoiceAllocator::GroupRequest request{uint8_t(part),uint8_t(variant*3),uint8_t(variant*13),uint8_t(variant*9),uint8_t(1+(variant&1))};
                write(state);
                MCU_Write(cpu,0xa3d0,request.part); MCU_Write(cpu,0xa3d1,request.fieldA3D1);
                MCU_Write(cpu,0xa3d2,request.value); MCU_Write(cpu,0xa1bf,request.fieldA1BF);
                MCU_Write(cpu,0xa3d4,request.voiceCount);
                for (auto& r : cpu.r) r = 0;
                cpu.r[7] = 0x9300;
                execute(0x1bab,0x1c3f);
                const auto result = state.createGroup(request);
                if (!result || result->group != MCU_Read(cpu,0xa3d5) || cpu.r[7] != 0x9300)
                    throw std::runtime_error("Group creation result differs");
                for (unsigned i = 0; i <= request.voiceCount; ++i)
                    if (result->voices[i] != MCU_Read(cpu,0xa3d6+i)) throw std::runtime_error("Group voice order differs");
                compare(state); ++createChecks;
            }
    std::printf("Native group creation: %u cases matched\n",createChecks);
    const auto index = [](unsigned n) { return uint8_t(n < 24 ? n : n == 24 ? 0xff : 0x80); };
    for (unsigned voice = 0; voice < 24; ++voice)
        for (unsigned next = 0; next < 26; ++next)
        {
            sc55::VoiceAllocator state;
            state.freeHead = uint8_t(voice); state.freeNext[voice] = index(next);
            state.freeTail = uint8_t((voice+5)%24); state.freeCount = uint8_t(next);
            write(state); cpu.r[0] = cpu.r[1] = 0;
            execute(0x1ca5,0x1cbc);
            const auto result = state.takeFreeVoice();
            if (!result || *result != cpu.r[1]) throw std::runtime_error("Free voice selection differs");
            compare(state); ++takeChecks;
        }
    for (unsigned voice = 0; voice < 24; ++voice)
        for (unsigned first = 0; first < 26; ++first)
            for (unsigned second = 0; second < 26; ++second)
                for (unsigned tail = 0; tail < 3; ++tail)
                {
                    sc55::VoiceAllocator state;
                    const unsigned group = (voice+first)%24, part = second%16;
                    for (unsigned i = 0; i < 24; ++i)
                    {
                        state.pcmLinks.first[i] = uint8_t((i+3)%24);
                        state.pcmLinks.second[i] = uint8_t((i+7)%24);
                        state.groups.next[i] = uint8_t((i+1)%24);
                        state.groups.previous[i] = uint8_t((i+2)%24);
                    }
                    state.pcmLinks.first[voice] = index(first); state.pcmLinks.second[voice] = index(second);
                    state.groups.tail[group] = tail == 0 ? 0xff : tail == 1 ? uint8_t((voice+1)%24) : uint8_t(voice);
                    write(state); cpu.r[0] = 0; cpu.r[1] = uint16_t(voice);
                    cpu.r[2] = uint16_t(group); cpu.r[3] = uint16_t(part);
                    execute(0x1c40,0x1ca4);
                    if (!state.attachVoice(voice,group,part)) throw std::runtime_error("Voice attach rejected valid input");
                    compare(state); ++attachChecks;
                }
    std::printf("Native free voice selection: %u cases matched; attach: %u matched\n",takeChecks,attachChecks);
    unsigned checks = 0, reclaimChecks = 0;
    for (unsigned voice = 0; voice < 24; ++voice)
        for (unsigned part = 0; part < 16; ++part)
            for (unsigned variant = 0; variant < 64; ++variant)
            {
                sc55::VoiceAllocator state;
                const auto group = (voice+7)%24, otherGroup = (group+1)%24, otherVoice = (voice+1)%24;
                state.groupNext.fill(0xff); state.groupPrevious.fill(0xff);
                state.partHead.fill(0xff); state.partTail.fill(0xff);
                state.voicePart[voice] = uint8_t(part); state.voiceGroup[voice] = uint8_t(group);
                state.status[voice] = (variant & 32) ? 0x94 : 0x14;
                state.fieldA360[voice] = 0x73; state.fieldA3E0[voice] = 0x65;
                state.freeTail = (variant & 1) ? uint8_t((voice+2)%24) : uint8_t(variant & 2 ? 0x80 : 0xff);
                state.freeHead = state.freeTail; state.freeCount = uint8_t(variant);
                state.freeGroupHead = uint8_t((group+2)%24);
                state.groups.head[group] = state.groups.tail[group] = uint8_t(voice);
                if (variant & 4)
                {
                    state.groups.next[voice] = uint8_t(otherVoice);
                    state.groups.previous[otherVoice] = uint8_t(voice);
                    state.groups.tail[group] = uint8_t(otherVoice);
                }
                state.partHead[part] = state.partTail[part] = uint8_t(group);
                if (variant & 8)
                {
                    state.groupNext[group] = uint8_t(otherGroup);
                    state.groupPrevious[otherGroup] = uint8_t(group);
                    state.partTail[part] = uint8_t(otherGroup);
                }
                else if (variant & 2)
                {
                    state.groupPrevious[group] = uint8_t(otherGroup);
                    state.groupNext[otherGroup] = uint8_t(group);
                    state.partHead[part] = uint8_t(otherGroup);
                }
                state.groupValue[group] = uint8_t(voice+20);
                state.groupValue[otherGroup] = uint8_t(127-voice);
                state.partMinimum[part] = variant & 16 ? 0xff : state.groupValue[group];
                state.partVoiceCount[part] = uint8_t(variant);
                state.activity[voice] = uint8_t(variant*3); state.shortage = uint8_t(variant);
                const auto originalState = state;
                visitAllocatorBytes(state,[&](unsigned address,uint8_t value) { MCU_Write(cpu,address,value); });
                cpu.cp = cpu.dp = cpu.ep = 0; cpu.pc = 0x07d0; cpu.sr = 0;
                MCU_Write16(cpu,0xa1d4,uint16_t(voice));
                for (auto& r : cpu.r) r = 0;
                cpu.r[1] = uint16_t(voice); cpu.r[7] = 0x9300;
                unsigned instructions = 0;
                while (cpu.pc != 0x07e6)
                {
                    if (++instructions > 300) throw std::runtime_error("Voice return escaped");
                    const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                }
                if (!state.returnVoice(voice) || cpu.r[7] != 0x9300)
                    throw std::runtime_error("Voice return rejected valid input or unbalanced stack");
                visitAllocatorBytes(state,[&](unsigned address,uint8_t value) {
                    if (value != MCU_Read(cpu,address))
                        throw std::runtime_error("Voice return mismatch at " + std::to_string(address)
                            + " variant " + std::to_string(variant));
                });
                ++checks;
                for (unsigned prepend = 0; prepend < 2; ++prepend)
                {
                    state = originalState;
                    write(state);
                    cpu.r[1] = uint16_t(voice); cpu.r[2] = uint16_t(group); cpu.r[3] = uint16_t(part); cpu.r[7] = 0x9300;
                    execute(prepend ? 0x1b24 : 0x1a7d,prepend ? 0x1baa : 0x1b0f);
                    if (!state.reclaimStoppedVoice(voice,group,part,prepend != 0) || cpu.r[7] != 0x9300)
                        throw std::runtime_error("Stopped voice reclaim failed");
                    compare(state); ++reclaimChecks;
                }
            }
    std::printf("Native voice return: %u cases matched\n",checks);
    std::printf("Native stopped voice reclaim: %u cases matched\n",reclaimChecks);
}
