#pragma once
#include "sc55_voice_lifecycle.h"
#include "sc55_voice_setup.h"
inline void TracePartialTransitions(Emulator& emu)
{
    auto& cpu=emu.GetMCU(); unsigned program=0,velocity=0,overwrites=0;
    const auto run=[&](unsigned cycles) {
        const auto end=cpu.cycles+cycles;
        while(cpu.cycles<end) {
            if(cpu.cp==0 && cpu.pc==0x11d0) {
                const auto a=MCU_Read(cpu,0xa3d6),b=MCU_Read(cpu,0xa3d7);
                if((MCU_Read(cpu,0xa1b0)&3)==3 && a<24 && (b>=128 || b==a)) {
                    ++overwrites;
                    std::printf("OVERWRITE program=%u velocity=%u slot=%u flags=%02x\n",
                        program,velocity,a,MCU_Read(cpu,0xa1b1));
                }
            }
            emu.Step();
        }
    };
    run(120000000);
    const auto send=[&](std::initializer_list<uint8_t> b) {
        emu.PostMIDI(std::span(b.begin(),b.size())); run(200000);
    };
    for(program=0;program<128;++program) {
        send({0xb0,126,1}); send({0xc0,uint8_t(program)}); send({0xb0,65,127});
        velocity=1; send({0x90,60,1}); velocity=127; send({0x90,67,127});
        send({0x80,67,0}); send({0x80,60,0});
    }
    std::printf("Partial transitions: %u same-slot preparations\n",overwrites);
}
inline void TraceHighNotes(Emulator& emu)
{
    auto& cpu=emu.GetMCU();
    unsigned program=0,key=0,present=0,absent=0;
    std::optional<sc55::HighNoteMapping> highMapping;
    const auto run=[&](unsigned cycles) {
        const auto end=cpu.cycles+cycles;
        while(cpu.cycles<end) {
            if(cpu.cp==0 && cpu.pc==0x0ccd) {
                std::array<uint8_t,20> common{};
                const auto address=(unsigned(cpu.ep)<<16)+unsigned(cpu.r[4])*216+12;
                for(unsigned i=0;i<20;++i) common[i]=MCU_Read(cpu,address+i);
                highMapping=sc55::MapHighNote(uint8_t(cpu.r[1]),common);
            }
            if(cpu.cp==0 && cpu.pc==0x0ce8) {
                const auto tone=cpu.r[4];
                if(!highMapping || highMapping->tone!=tone || highMapping->note!=MCU_Read(cpu,0xa1b2))
                    throw std::runtime_error("Native high-note mapping differs from H8");
                highMapping.reset();
                if(tone&0x8000) ++absent; else ++present;
                std::printf("HIGH program=%u key=%u tone=%04x pitch=%u\n",program,key,tone,MCU_Read(cpu,0xa1b2));
            }
            if(cpu.cp==0 && cpu.pc==0x11ea)
                std::printf("HIGH-START key=%u groupKey=%u flags=%02x selector=%02x\n",
                    MCU_Read(cpu,0xa1b2),MCU_Read(cpu,0xa3d2),MCU_Read(cpu,0xa1b1),MCU_Read(cpu,0xa3d1));
            emu.Step();
        }
    };
    run(120000000);
    const auto send=[&](std::initializer_list<uint8_t> b) {
        emu.PostMIDI(std::span(b.begin(),b.size())); run(200000);
    };
    for(program=0;program<128;++program) {
        send({0xc0,uint8_t(program)});
        for(key=125;key<128;++key) {
            send({0x90,uint8_t(key),80}); send({0x80,uint8_t(key),0});
        }
    }
    if(present+absent!=384) throw std::runtime_error("Missing high-note observations");
    std::printf("High notes: %u present, %u absent\n",present,absent);
}
// Real MIDI lifecycle probe: no native shortcuts, synthetic RAM state or PCM
// interception. Observe the installed restart flag at the pitch branch.
inline void TraceMonoPitch(Emulator& emu)
{
    auto& cpu=emu.GetMCU();
    unsigned initialized=0,reentered=0;
    unsigned reuseChecks=0;
    unsigned sourceChecks=0;
    std::optional<uint8_t> expectedSource;
    std::optional<uint8_t> expectedGroup;
    unsigned groupChecks=0;
    struct ReuseCheck {
        unsigned address;
        sc55::VoiceStopState voice;
        sc55::SecondEnvelopePcmState second;
        sc55::PreparedVoicePcm prepared;
        sc55::VoicePostEnable post;
    };
    std::optional<ReuseCheck> expected;
    const auto run=[&](unsigned cycles,const char* label) {
        const auto end=cpu.cycles+cycles;
        while(cpu.cycles<end) {
            if(cpu.cp==0 && cpu.pc==0x0d4e) {
                sc55::VoiceAllocator allocator;
                const auto part=cpu.r[3]&15;
                allocator.partTail[part]=MCU_Read(cpu,0xa220+part);
                for(unsigned g=0;g<24;++g) {
                    allocator.noteGroups[g].previous=MCU_Read(cpu,0xa258+g);
                    allocator.noteGroups[g].key=MCU_Read(cpu,0xa2e8+g);
                    allocator.noteGroups[g].noteClass=MCU_Read(cpu,0xa2d0+g);
                }
                expectedGroup=allocator.findSourceGroup(part,uint8_t(cpu.r[4]));
                if(!expectedGroup) throw std::runtime_error("Invalid source-group links");
            }
            if(cpu.cp==0 && expectedGroup && (cpu.pc==0x0d76 || cpu.pc==0x0dcc)) {
                const auto actual=cpu.pc==0x0d76 ? uint8_t(cpu.r[2]) : uint8_t(255);
                if(*expectedGroup!=actual) throw std::runtime_error("Native source-group search differs from H8");
                expectedGroup.reset(); ++groupChecks;
            }
            if(cpu.cp==0 && (cpu.pc==0x1dd0 || cpu.pc==0x1df7)) {
                const auto partial=(unsigned(cpu.ep)<<16)|cpu.r[5];
                expectedSource=sc55::PreparePortamentoSourceKey(uint8_t(cpu.r[0]),
                    MCU_Read(cpu,0xa1b4),MCU_Read(cpu,partial+10),MCU_Read(cpu,partial+13));
                if(!expectedSource) throw std::runtime_error("Invalid live source pitch");
                std::printf("SOURCE-KEY %s input=%u reference=%u coarse=%u tracking=%u expected=%u\n",
                    label,cpu.r[0]&255,MCU_Read(cpu,0xa1b4),MCU_Read(cpu,partial+10),
                    MCU_Read(cpu,partial+13),*expectedSource);
            }
            if(cpu.cp==0 && (cpu.pc==0x1dd6 || cpu.pc==0x1dfd)) {
                if(!expectedSource || *expectedSource!=(cpu.r[0]&255))
                    throw std::runtime_error("Native portamento source differs from H8");
                expectedSource.reset(); ++sourceChecks;
            }
            if(cpu.cp==0 && cpu.pc==0x2c18) {
                if(expected) throw std::runtime_error("Nested reuse preparation");
                expected.emplace(); auto& e=*expected; e.address=cpu.r[0];
                e.voice.flagMinus3B=MCU_Read(cpu,e.address-59);
                e.voice.stages[0]=MCU_Read16(cpu,e.address);
                e.voice.savedStage=MCU_Read16(cpu,e.address+6);
                e.second.level=MCU_Read16(cpu,e.address+36);
                std::vector<std::pair<uint8_t,uint8_t>> writes;
                const auto slot=MCU_Read16(cpu,e.address-2);
                if(!sc55::PrepareReusedVoicePcm(slot,e.voice,MCU_Read16(cpu,e.address+28),
                    e.second,e.prepared,e.post,[&](uint8_t a,uint8_t v) { writes.emplace_back(a,v); })
                    || writes!=std::vector<std::pair<uint8_t,uint8_t>>{{0x3e,uint8_t(slot)},{0x16,0},{0x17,0xb4}})
                    throw std::runtime_error("Native reuse freeze rejected live state");
            }
            if(cpu.cp==0 && cpu.pc==0x2c52 && expected) {
                const auto& e=*expected; const auto a=e.address;
                if(MCU_Read16(cpu,a)!=e.voice.stages[0] || MCU_Read16(cpu,a+6)!=e.voice.savedStage
                    || MCU_Read16(cpu,a+26)!=e.voice.cached16 || MCU_Read16(cpu,a+30)!=e.voice.cached18
                    || MCU_Read16(cpu,a+38)!=e.second.command || MCU_Read16(cpu,a-22)!=e.post.level
                    || MCU_Read16(cpu,a-24)!=e.prepared.pcm1a)
                    throw std::runtime_error("Native reuse freeze differs from H8");
                auto repeated=e;
                if(!sc55::PrepareReusedVoicePcm(MCU_Read16(cpu,a-2),repeated.voice,MCU_Read16(cpu,a+28),
                    repeated.second,repeated.prepared,repeated.post,[](uint8_t,uint8_t) {})
                    || repeated.voice.savedStage!=e.voice.savedStage)
                    throw std::runtime_error("Repeated reuse lost saved stage");
                sc55::ActivatePreparedVoice(repeated.voice,[](uint8_t,uint8_t) {});
                if(repeated.voice.stages[0]!=e.voice.savedStage || repeated.voice.savedStage!=0)
                    throw std::runtime_error("Reuse activation did not restore stage");
                expected.reset(); ++reuseChecks;
            }
            if (cpu.cp==0 && cpu.pc==0x4f51) {
                const unsigned voice=cpu.r[0];
                const auto flags=MCU_Read(cpu,voice-59);
                if(flags&0x80) ++initialized; else ++reentered;
                std::printf("MONO %s slot=%u flags=%02x stage=%u progress=%u elapsed=%u current=%u velocity=%u\n",
                    label,MCU_Read16(cpu,voice-2),flags,MCU_Read16(cpu,voice+4),
                    MCU_Read16(cpu,voice+12),MCU_Read16(cpu,0xac5a),
                    MCU_Read(cpu,0xa071),MCU_Read(cpu,0xa3d3));
            }
            if (cpu.cp==0 && cpu.pc==0x11d0)
                std::printf("MONO-PLAN %s flags=%02x slots=%u/%u previous=%04x newGroup=%04x\n",
                    label,MCU_Read(cpu,0xa1b1),MCU_Read(cpu,0xa3d6),MCU_Read(cpu,0xa3d7),
                    MCU_Read16(cpu,0xa1cc),MCU_Read16(cpu,0xa1ce));
            if (cpu.cp==0 && cpu.pc==0x1d9b)
                std::printf("SOURCE %s key=%u latch=%u source=%u\n",label,
                    MCU_Read(cpu,0xa1b2),MCU_Read(cpu,0xa050+(cpu.r[3]&15)),cpu.r[1]&255);
            const auto oldInvalidation=MCU_Read16(cpu,0xa1ce);
            const auto oldPc=cpu.pc; const auto oldCp=cpu.cp;
            emu.Step();
            const auto newInvalidation=MCU_Read16(cpu,0xa1ce);
            if(oldInvalidation!=newInvalidation)
                std::printf("INVALIDATE %s %02x:%04x %04x->%04x\n",label,oldCp,oldPc,oldInvalidation,newInvalidation);
        }
    };
    run(120000000,"boot"); initialized=reentered=0;
    const auto send=[&](std::initializer_list<uint8_t> bytes,const char* label) {
        emu.PostMIDI(std::span(bytes.begin(),bytes.size())); run(1000000,label);
    };
    send({0xc0,80},"program");
    send({0xb0,126,1},"mono");
    send({0x90,60,40},"first60");
    send({0x90,64,80},"legato64");
    send({0x90,67,110},"legato67");
    send({0x80,67,0},"return64");
    send({0x80,64,0},"return60");
    send({0x80,60,0},"off");
    run(10000000,"drain");
    send({0xb0,65,127},"portamento");
    send({0x90,60,40},"porta60");
    send({0x90,67,110},"porta67");
    send({0x80,67,0},"portaReturn60");
    send({0x80,60,0},"portaOff");
    run(10000000,"drain");
    send({0xc0,73},"singleProgram");
    send({0x90,60,80},"single60");
    send({0xc0,80},"pairedProgram");
    send({0x90,67,110},"singleToPair67");
    send({0x80,67,0},"singleToPairReturn60");
    send({0x80,60,0},"singleToPairOff");
    run(10000000,"drain");
    const auto source=[&](uint8_t value,const char* label) {
        if(MCU_Read(cpu,0xa051)!=value)
            throw std::runtime_error(std::string("Source latch mismatch: ")+label);
    };
    for (bool poly : {false,true}) {
        send({0xb0,uint8_t(poly ? 127 : 126),0},poly ? "sourcePoly" : "sourceMono");
        send({0xb0,65,0},"sourcePortaOff");
        send({0xb0,84,48},"source48"); source(48,"CC84");
        send({0xb0,65,0},"sourceSwitchOff"); source(48,"CC65 off preserves");
        send({0x90,60,80},poly ? "polySource60" : "monoSource60"); source(255,"note consumes");
        send({0xb0,84,60},"source60");
        send({0x90,67,100},poly ? "polySource67" : "monoSource67"); source(255,"legato consumes");
        send({0x80,67,0},"sourceOff67");
        send({0x80,60,0},"sourceOff60");
        send({0xb0,84,48},"source48");
        send({0xb0,65,127},"sourceSwitchOn"); source(255,"CC65 on clears");
        send({0xb0,84,48},"source48");
        send({0xb0,121,0},"sourceReset"); source(255,"CC121 clears");
        run(10000000,"drain");
        if(poly) {
            send({0x90,60,80},"chord60"); send({0x90,64,80},"chord64");
            send({0x90,72,80},"chord72");
            send({0xb0,84,64},"middleSource64"); send({0x90,67,100},"middleDestination67");
            send({0x80,64,0},"oldSourceOff"); send({0x80,67,0},"destinationOff");
            send({0x80,60,0},"chordOff60"); send({0x80,72,0},"chordOff72");
        }
    }
    send({0xb0,126,1},"historyMono"); send({0xc0,80},"historyProgram80");
    send({0x90,60,80},"historyNote60");
    if(MCU_Read16(cpu,0xa1ce)&2) throw std::runtime_error("Initial mono invalidation not consumed");
    send({0xc0,73},"historyProgram73"); send({0xc0,80},"historyProgramBack80");
    if(!(MCU_Read16(cpu,0xa1ce)&2)) throw std::runtime_error("Program round trip lost invalidation");
    send({0x90,67,80},"historyNote67");
    if(MCU_Read16(cpu,0xa1ce)&2) throw std::runtime_error("Mono note did not consume invalidation");
    send({0xc0,80},"historySameProgram80");
    if(MCU_Read16(cpu,0xa1ce)&2) throw std::runtime_error("Same program invalidated reuse");
    send({0x90,69,80},"historyNote69");
    send({0xb0,127,0},"polyHistoryMode"); send({0xb0,65,127},"polyHistoryOn");
    send({0x90,48,80},"polyHistory48");
    if(MCU_Read16(cpu,0xa192)!=0x3030) throw std::runtime_error("Poly history48 differs");
    send({0x90,72,80},"polyHistory72");
    if(MCU_Read16(cpu,0xa192)!=0x4848) throw std::runtime_error("Poly history72 differs");
    send({0xb0,121,0},"polyHistoryResetControllers");
    if(MCU_Read16(cpu,0xa192)!=0x4848) throw std::runtime_error("CC121 erased pitch history");
    if(!initialized || !reentered) throw std::runtime_error("Mono probe missed a pitch preparation branch");
    if(expected || !reuseChecks) throw std::runtime_error("Missing reuse freeze comparison");
    if(expectedSource || sourceChecks!=10) throw std::runtime_error("Missing source pitch comparisons");
    if(expectedGroup || groupChecks!=3) throw std::runtime_error("Missing source group comparisons");
    std::printf("Poly source: %u native group searches matched H8\n",groupChecks);
    std::printf("Portamento source: %u partial pitch calculations matched H8; mono/poly latch checks passed\n",sourceChecks);
    std::printf("Mono pitch observed: %u initializations, %u reentries; %u native reuse freezes matched H8\n",
        initialized,reentered,reuseChecks);
}
