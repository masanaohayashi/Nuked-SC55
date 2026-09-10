#pragma once
#include "sc55_synth.h"

// Compare event ordering, not a fitted MIDI or task latency. Feed a real CC
// after each engine has entered its own physical-voice reuse wait.
inline void VerifyMidiDuringPreparation(Emulator& emu,const RomsetInfo& roms,bool programChange=false,bool modeChange=false,bool systemExclusive=false,bool partExclusive=false)
{
    // The same queue used by the player: complete fan-out publication, owned
    // snapshots, FIFO wraparound and full-queue retry without partial commits.
    sc55::VoiceCommands requests;
    std::array<sc55::VoiceCommand,16> batch{};
    for(unsigned i=0;i<batch.size();++i)
        batch[i]=sc55::NoteRequest{sc55::NoteRequest::Action::on,uint8_t(15-i),uint8_t(48+i),uint8_t(64+i),uint16_t(100+i)};
    const auto expected=batch;
    for(unsigned i=0;i<8;++i)
        if(!requests.publish(batch)) throw std::runtime_error("Note request batch rejected before full");
    std::get<sc55::NoteRequest>(batch[0]).key=127;
    std::get<sc55::NoteRequest>(batch[0]).tone=999;
    if(requests.publish(batch) || requests.size()!=128)
        throw std::runtime_error("Full note queue partially published a fan-out");
    for(unsigned i=0;i<128;++i) {
        const auto command=requests.take();
        const auto* request=command ? std::get_if<sc55::NoteRequest>(&*command) : nullptr;
        const auto& original=std::get<sc55::NoteRequest>(expected[i%16]);
        if(!request || request->part!=original.part || request->key!=original.key
            || request->velocity!=original.velocity || request->tone!=original.tone)
            throw std::runtime_error("Note queue reordered or borrowed receive state");
        if(i==0 && requests.publish(batch))
            throw std::runtime_error("Note queue accepted a partial fan-out with one free entry");
    }
    if(requests.take() || !requests.publish(batch) || std::get<sc55::NoteRequest>(*requests.take()).key!=127)
        throw std::runtime_error("Note queue did not recover from backpressure");
    requests.clear();
    const std::array<sc55::VoiceCommand,4> mixed{
        sc55::PedalRequest{sc55::PedalRequest::Kind::sostenuto,2,true},
        sc55::NoteRequest{sc55::NoteRequest::Action::off,2,60,0,100},
        sc55::PortamentoSourceRequest{2,48},
        sc55::PartReleaseRequest{sc55::PartReleaseRequest::Kind::sound,2}};
    if(!requests.publish(mixed)) throw std::runtime_error("Mixed command queue rejected input");
    for(const auto& expectedCommand:mixed) {
        const auto command=requests.take();
        if(!command || command->index()!=expectedCommand.index())
            throw std::runtime_error("Voice command types were reordered");
    }
    const auto& raw=roms.rom_data;
    const auto& r1=raw[size_t(RomLocation::ROM1)];const auto& r2=raw[size_t(RomLocation::ROM2)];
    sc55::SoundData data;
    if(!data.loadEncoded(sc55::ImportSoundData(r1,r2))) throw std::runtime_error("Invalid sound data");
    auto pcm=std::make_unique<pcm_t>();pcm->is_mk1=true;
    std::copy_n(emu.GetPCM().waverom1,sizeof(pcm->waverom1),pcm->waverom1);
    std::copy_n(emu.GetPCM().waverom2,sizeof(pcm->waverom2),pcm->waverom2);
    std::copy_n(emu.GetPCM().waverom3,sizeof(pcm->waverom3),pcm->waverom3);
    sc55::NativeMelodicPlayer player(data,*pcm,sc55::ImportSystemDefaults(r1,r2),
        sc55::ImportRhythmPresets(r1,r2),sc55::ImportMelodicPresets(r1,r2),sc55::ImportEffectsTables(r1,r2));
    auto& cpu=emu.GetMCU();
    const auto runH8=[&](uint64_t cycles) {const auto end=cpu.cycles+cycles;while(cpu.cycles<end) emu.Step();};
    runH8(120000000);player.renderFrames(16384);
    const auto send=[&](std::span<const uint8_t> bytes) {
        emu.PostMIDI(bytes);player.push(bytes);runH8(4000000);player.renderFrames(6400);
    };
    const uint8_t program[]{0xc0,80};send(program);
    for(uint8_t key=48;key<72;++key) {const uint8_t note[]{0x90,key,100};send(note);}
    if(player.freeVoices()!=0 || MCU_Read(cpu,0xa3c1)!=0)
        throw std::runtime_error("MIDI-during-preparation fixture is not at full capacity");
    const uint8_t next[]{0x90,72,100},controls[]{0x90,73,100,0xb0,121,0,0xb0,11,23,0xb0,64,127,0xb0,7,17};
    const uint8_t programs[]{0x90,73,100,0xc0,48,0xb0,7,17};
    const uint8_t modes[]{0xb0,126,0,0xb0,7,17};
    const uint8_t exclusive[]{0xf0,0x41,0x10,0x42,0x12,0x40,0,4,17,43,0xf7,0xb0,7,17};
    const uint8_t partWrite[]{0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0x19,17,5,0xf7,0xb0,7,17};
    if(systemExclusive || partExclusive) {
        // A whole SysEx takes longer on the UART than one reuse wait. Build
        // a real note backlog, rather than changing H8 timing or RAM.
        std::vector<uint8_t> input;
        for(uint8_t key=72;key<104;++key) input.insert(input.end(),{0x90,key,100});
        const auto packet=partExclusive ? std::span<const uint8_t>(partWrite) : std::span<const uint8_t>(exclusive);
        input.insert(input.end(),packet.begin(),packet.end());
        emu.PostMIDI(input);player.push(input);
        bool h8Applied=false,nativeApplied=false;
        unsigned h8Read=0,h8Write=0,nativeOutstanding=0;
        const auto end=cpu.cycles+8000000;
        while(cpu.cycles<end) {
            if(!h8Applied && MCU_Read(cpu,partExclusive ? 0x80c0 : 0x8002)==17) {
                h8Applied=true;
                h8Read=MCU_Read(cpu,0xaaf8);h8Write=MCU_Read(cpu,0xaaf9);
            }
            emu.Step();
        }
        for(unsigned frame=0;frame<12800;++frame) {
            player.renderFrames(1);
            if(!nativeApplied && (partExclusive ? player.partSettings().parts[1].controls.volume : player.masterControls().volume)==17) {
                nativeApplied=true;nativeOutstanding=unsigned(player.queuedEvents());
            }
        }
        std::printf("SysEx %s write: applied H8/native=%u/%u H8 command cursors=%02x/%02x native queued=%u\n",
            partExclusive ? "part" : "master",h8Applied,nativeApplied,h8Read,h8Write,nativeOutstanding);
        std::fflush(stdout);
        if(!h8Applied || h8Read==h8Write) throw std::runtime_error("H8 SysEx fixture did not overlap note work");
        if(!nativeApplied || !nativeOutstanding) throw std::runtime_error("Native SysEx waited for all note work");
        if(player.failed() || player.queuedEvents()!=0 || player.partSettings().parts[1].controls.volume!=17)
            throw std::runtime_error("SysEx/following controller did not complete");
        if(partExclusive) {
            const uint8_t hold[]{0xb0,64,127,0x90,60,100};send(hold);
            // Assign the SAME channel:04:1194 resets controller inputs, then
            // publishes reset and All Notes Off in that order, unconditionally.
            const uint8_t channel[]{0xf0,0x41,0x10,0x42,0x12,0x40,0x11,2,0,45,0xf7,0xb0,11,23};
            send(channel);runH8(8000000);player.renderFrames(12800);
            if(player.partSettings().parts[1].controls.expression!=23 || MCU_Read(cpu,0xab37)!=23
                || player.freeVoices()!=24 || MCU_Read(cpu,0xa3c1)!=24)
                throw std::runtime_error("GS channel change did not release held voices or preserve later expression");
            std::puts("Part SysEx during note work and same-channel release/reset PASS");
            return;
        }
        // A partial/invalid packet must not publish settings, nor hold later
        // MIDI forever. Exercise the same receiver via fragmented host input.
        const uint8_t prefix[]{0xf0,0x41,0x10,0x42,0x12,0x40,0,4,23};
        emu.PostMIDI(prefix);player.push(prefix);runH8(2000000);player.renderFrames(3200);
        if(player.masterControls().volume!=17 || MCU_Read(cpu,0x8002)!=17)
            throw std::runtime_error("Partial SysEx committed settings");
        const uint8_t badEnd[]{0,0xf7,0xb0,7,18}; // Deliberately incorrect checksum.
        send(badEnd);
        if(player.masterControls().volume!=17 || MCU_Read(cpu,0x8002)!=17
            || player.partSettings().parts[1].controls.volume!=18 || MCU_Read(cpu,0x80c0)!=18)
            throw std::runtime_error("Invalid SysEx changed settings or blocked following MIDI");
        emu.PostMIDI(prefix);player.push(prefix);runH8(2000000);player.renderFrames(3200);
        const uint8_t validEnd[]{37,0xf7,0xb0,7,19};
        send(validEnd);
        if(player.masterControls().volume!=23 || MCU_Read(cpu,0x8002)!=23
            || player.partSettings().parts[1].controls.volume!=19 || MCU_Read(cpu,0x80c0)!=19)
            throw std::runtime_error("Fragmented SysEx did not recover after rejection");
        std::puts("SysEx during queued note preparation PASS");
        return;
    }
    const auto volume=modeChange ? std::span<const uint8_t>(modes)
        : programChange ? std::span<const uint8_t>(programs) : std::span<const uint8_t>(controls);
    emu.PostMIDI(next);
    bool waiting=false,returned=false,h8Applied=false;
    const auto limit=cpu.cycles+4000000;
    while(cpu.cycles<limit) {
        if(cpu.cp==0 && cpu.pc==0x5710 && !waiting) {
            const auto slot=MCU_Read16(cpu,uint16_t(cpu.r[1]-2));
            if(slot<24 && emu.GetPCM().ram2[slot][9] && emu.GetPCM().ram2[slot][10]) {
                waiting=true;emu.PostMIDI(volume);
            }
        }
        if(waiting) {
            h8Applied|=MCU_Read(cpu,0x80c0)==17;
            // A mode command stops the preparing voice, so task2 need not
            // return through the normal gain-ready exit at573e.
            if(modeChange && h8Applied) {returned=true;break;}
            if(cpu.cp==0 && cpu.pc==0x573e) {returned=true;break;}
        }
        emu.Step();
    }
    if(!waiting || !returned || !h8Applied) {
        std::printf("Input wait probe: waiting=%u returned=%u applied=%u free=%u mode=%02x\n",
            waiting,returned,h8Applied,MCU_Read(cpu,0xa3c1),MCU_Read(cpu,0x80bd));
        std::fflush(stdout);
        throw std::runtime_error("H8 did not demonstrate CC application during reuse wait");
    }
    player.push(next);
    using Status=sc55::VoiceControlRuntime::StartStatus;
    // The previous note's protected calculation may still be running.
    // Trigger the controller input at the actual reuse wait, like the H8 side.
    for(unsigned frame=0;frame<6400 && player.startupAudit().status!=Status::waitingForReuse && !player.failed();++frame)
        player.renderFrames(1);
    if(player.startupAudit().status!=Status::waitingForReuse)
        throw std::runtime_error("Native fixture did not enter reuse wait");
    player.push(volume);
    bool nativeApplied=false;
    for(unsigned frame=0;frame<6400 && player.startupAudit().status==Status::waitingForReuse;++frame) {
        nativeApplied|=player.partSettings().parts[1].controls.volume==17;
        player.renderFrames(1);
    }
    if(player.failed()) throw std::runtime_error("MIDI during preparation failed native engine");
    std::printf(modeChange ? "Mode input during preparation, following CC7 handled: H8=%u native=%u\n"
        : "CC7 applied before reuse finished: H8=%u native=%u\n",h8Applied,nativeApplied);
    std::fflush(stdout);
    if(!nativeApplied) throw std::runtime_error("Native preparation blocked independent MIDI interpretation");
    if(!programChange && !modeChange && (player.partSettings().parts[1].controls.expression!=23 || MCU_Read(cpu,0xab37)!=23))
        throw std::runtime_error("Controller reset delayed later expression input");
    player.renderFrames(6400);
    if(modeChange) {
        runH8(4000000);
        if(player.failed() || player.queuedEvents()!=0 || player.freeVoices()!=24 || MCU_Read(cpu,0xa3c1)!=24
            || (player.partSettings().routing[1].noteFlags&0x80) || (MCU_Read(cpu,0x80bd)&0x80))
            throw std::runtime_error("Mono receive/voice-side stop differs from H8");
        const uint8_t first[]{0x90,60,100},second[]{0x90,64,100},poly[]{0xb0,127,0};
        const auto compareGroups=[&](unsigned expectedCount) {
            std::vector<uint8_t> h8Keys,nativeKeys;
            const auto& allocator=player.allocatorAudit();
            for(unsigned group=allocator.partHead[1],n=0;group<24 && n<24;
                group=allocator.noteGroups[group].next,++n) nativeKeys.push_back(allocator.noteGroups[group].key);
            for(unsigned group=MCU_Read(cpu,0xa211),n=0;group<24 && n<24;
                group=MCU_Read(cpu,0xa240+group),++n) h8Keys.push_back(MCU_Read(cpu,0xa2e8+group));
            std::sort(h8Keys.begin(),h8Keys.end());std::sort(nativeKeys.begin(),nativeKeys.end());
            // Mono means one note group, not one physical partial.
            return nativeKeys.size()==expectedCount && nativeKeys==h8Keys
                && player.freeVoices()==MCU_Read(cpu,0xa3c1);
        };
        send(first);send(second);
        if(!compareGroups(1)) throw std::runtime_error("Deferred mono switch did not retain one note group");
        send(poly);send(first);send(second);
        if(!compareGroups(2))
            throw std::runtime_error("Deferred poly switch did not restore polyphony");
        send(poly);
        if(player.freeVoices()!=24 || MCU_Read(cpu,0xa3c1)!=24)
            throw std::runtime_error("Repeated MIDI poly command did not stop voices");
        std::puts("Mode input during reuse, mono/poly admission and repeated mode command PASS");
        return;
    }
    if(player.failed() || player.queuedEvents()!=0 || player.freeVoices()!=0)
        throw std::runtime_error("Pending notes did not finish after independent MIDI interpretation");
    if(programChange) {
        runH8(4000000);
        if(player.partSettings().parts[1].controls.program!=48 || MCU_Read(cpu,0x80b9)!=48
            || player.selectedTone(1)!=std::optional<uint16_t>(MCU_Read16(cpu,0xab08))
            || player.capacityPolicy().modes[1]!=MCU_Read(cpu,0xa041))
            throw std::runtime_error("Program receive/voice command state differs from H8");
        // Receive both drum requests while the kit is valid, then reject only
        // later input with an invalid kit. The already-queued snare survives.
        const uint8_t drums[]{0x99,36,100,0x99,38,100,0xc9,64};
        emu.PostMIDI(drums);player.push(drums);
        bool h8Snare=false,nativeSnare=false;
        const auto end=cpu.cycles+4000000;
        while(cpu.cycles<end) {
            if(cpu.cp==0 && cpu.pc==0x1177 && cpu.r[1]<24 && MCU_Read(cpu,0xc8e4+cpu.r[1])==0) {
                // Drum C8FC is the mapped envelope key, not the received
                // MIDI key. A1B6 is set by the drum admission at00:0c3e.
                h8Snare|=MCU_Read(cpu,0xa1b6)==38;
            }
            emu.Step();
        }
        for(unsigned frame=0;frame<6400;++frame) {
            player.renderFrames(1);
            const auto& allocator=player.allocatorAudit();
            for(const auto& allocation:allocator.allocations)
                if(!(allocation.status&0x80) && allocation.part==0 && allocation.noteGroup<24)
                    nativeSnare|=allocator.noteGroups[allocation.noteGroup].key==38;
        }
        std::printf("Drum program probe: snare H8/native=%u/%u failed=%u queued=%zu gate=%04x program=%u\n",
            h8Snare,nativeSnare,player.failed(),player.queuedEvents(),MCU_Read16(cpu,0xab06),
            player.partSettings().parts[0].controls.program);
        std::fflush(stdout);
        if(!h8Snare || !nativeSnare || player.failed() || player.queuedEvents()!=0
            || MCU_Read16(cpu,0xab06)!=0xffff || player.partSettings().parts[0].controls.program!=64)
            throw std::runtime_error("Later rejected drum program cancelled an accepted snare");
        std::puts("Accepted drum note survives later invalid kit selection: H8/native PASS");
        std::puts("Program input and deferred voice configuration during reuse PASS");
        return;
    }
    // The pedal queued behind the new note must subsequently own that note's
    // release. All Notes Off cannot bypass hold; hold-off must release it.
    const uint8_t notesOff[]{0xb0,123,0},holdOff[]{0xb0,64,0};
    send(notesOff);
    if(player.partSettings().parts[1].controls.expression!=23 || MCU_Read(cpu,0xab37)!=23)
        throw std::runtime_error("Deferred controller reset overwrote newer expression");
    if(player.freeVoices()!=0 || MCU_Read(cpu,0xa3c1)!=0)
        throw std::runtime_error("Queued hold failed to protect notes from All Notes Off");
    send(holdOff);runH8(80000000);player.renderFrames(128000);
    if(player.failed() || player.queuedEvents()!=0 || player.freeVoices()!=24 || MCU_Read(cpu,0xa3c1)!=24)
        throw std::runtime_error("Queued hold-off did not release the protected notes");
    std::puts("Voice commands: FIFO/fan-out snapshots/backpressure, MIDI during preparation and held release PASS");
}
