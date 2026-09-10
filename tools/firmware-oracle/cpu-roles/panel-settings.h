#pragma once
#include "sc55_synth.h"

// Observe option entry using physical switches only, never patched firmware RAM.
inline void InspectPanelOptions(Emulator& emu,const RomsetInfo& roms)
{
    const auto& raw=roms.rom_data;
    const auto& r1=raw[size_t(RomLocation::ROM1)];const auto& r2=raw[size_t(RomLocation::ROM2)];
    sc55::NativeSynth synth(sc55::ImportSoundData(r1,r2),r1,r2,
        raw[size_t(RomLocation::WAVEROM1)],raw[size_t(RomLocation::WAVEROM2)],raw[size_t(RomLocation::WAVEROM3)]);
    auto& cpu=emu.GetMCU();
    const auto run=[&](unsigned cycles,const char* label) {
        const auto end=cpu.cycles+cycles;
        while(cpu.cycles<end) emu.Step();
        std::array<AudioFrame<int32_t>,257> block{};
        for(unsigned count=cycles/625;count;) {
            const auto n=std::min(count,257u);synth.render(std::span(block).first(n));count-=n;
        }
        const auto state=synth.state();
        if(synth.failed() || synth.standby()!=(MCU_Read(cpu,0xcdca)==0)
            || synth.fastDisplayScroll()!=bool(MCU_Read16(cpu,0xcdc8)&8)
            || state.parts[1].voices!=MCU_Read(cpu,0xa1f1)
            || state.parts[1].volume!=MCU_Read(cpu,0x80c0))
            throw std::runtime_error("Native standby reception/voice state differs from H8");
        std::printf("Panel options %s: page=%u options=%04x voices=%u volume=%u\n",label,
            MCU_Read(cpu,0xcdca),MCU_Read16(cpu,0xcdc8),MCU_Read(cpu,0xa1f1),MCU_Read(cpu,0x80c0));
    };
    run(120000000,"boot");
    const uint8_t initialNote[]{0xc0,80,0xb0,7,100,0x90,60,100};
    emu.PostMIDI(initialNote);synth.push(initialNote);run(4000000,"note before standby");
    if(MCU_Read(cpu,0xa1f1)!=2) throw std::runtime_error("Standby fixture did not start a note");
    constexpr uint32_t power=1u<<MCU_BUTTON_POWER;
    constexpr uint32_t all=1u<<MCU_BUTTON_INST_ALL;
    constexpr uint32_t right=1u<<MCU_BUTTON_INST_R;
    synth.setStandby(true);cpu.button_pressed.store(power);run(2000000,"power held");
    const uint8_t standbyMidi[]{0xb0,7,27,0x90,67,100};
    emu.PostMIDI(standbyMidi);synth.push(standbyMidi);run(4000000,"MIDI during standby");
    if(MCU_Read(cpu,0xa1f1)!=0 || MCU_Read(cpu,0x80c0)!=100)
        throw std::runtime_error("Standby did not stop voices or discard MIDI");
    cpu.button_pressed.store(power|all);run(2000000,"power/all held");
    if(!synth.setFastDisplayScroll(true)) throw std::runtime_error("Standby option rejected");
    cpu.button_pressed.store(power|all|right);run(2000000,"power/all/right held");
    cpu.button_pressed.store(0);run(2000000,"release");
    synth.setStandby(false);cpu.button_pressed.store(power);run(2000000,"power again");
    cpu.button_pressed.store(0);run(4000000,"playing");
    const uint8_t followingNote[]{0x90,69,100};
    emu.PostMIDI(followingNote);synth.push(followingNote);run(4000000,"note after standby");
    if(MCU_Read(cpu,0xa1f1)!=2 || MCU_Read(cpu,0x80c0)!=100)
        throw std::runtime_error("Standby MIDI leaked or reception failed to resume");
    if(MCU_Read(cpu,0xcdca)!=1 || !(MCU_Read16(cpu,0xcdc8)&8))
        throw std::runtime_error("Fast scroll option did not survive power-on");
    synth.setStandby(true);cpu.button_pressed.store(power);run(2000000,"standby again");
    cpu.button_pressed.store(power|(1u<<MCU_BUTTON_INST_MUTE));
    run(2000000,"power/mute held");
    if(!synth.setFastDisplayScroll(false)) throw std::runtime_error("Standby option rejected");
    cpu.button_pressed.store(power|(1u<<MCU_BUTTON_INST_MUTE)|right);
    run(2000000,"power/mute/right held");
    cpu.button_pressed.store(0);run(2000000,"disabled release");
    synth.setStandby(false);cpu.button_pressed.store(power);run(2000000,"disabled power-on");
    cpu.button_pressed.store(0);run(4000000,"normal scroll");
    if(MCU_Read(cpu,0xcdca)!=1 || (MCU_Read16(cpu,0xcdc8)&8))
        throw std::runtime_error("Fast scroll option did not clear");
    // Product-state stress, not instruction/sample parity: switch while note
    // admission is at different render boundaries, with sustain and stealing.
    for(unsigned delay:{0u,1u,8u,127u,257u,4096u}) {
        sc55::NativeSynth trial(sc55::ImportSoundData(r1,r2),r1,r2,
            raw[size_t(RomLocation::WAVEROM1)],raw[size_t(RomLocation::WAVEROM2)],raw[size_t(RomLocation::WAVEROM3)]);
        const auto render=[&](unsigned count) {
            std::array<AudioFrame<int32_t>,127> block{};
            trial.render(std::span(block).first(0));
            while(count) {const auto n=std::min(count,127u);trial.render(std::span(block).first(n));count-=n;}
            if(trial.failed()) throw std::runtime_error("Standby during admission failed");
        };
        std::vector<uint8_t> held{0xc0,80,0xb0,64,127};
        for(unsigned key=40;key<72;++key) held.insert(held.end(),{0x90,uint8_t(key),100});
        trial.push(held);render(delay);
        trial.setStandby(true);
        trial.push(standbyMidi);render(16000);
        // The PCM enable mask can remain00ffffff with no allocated notes.
        // It is not a sounding-owner count; do not require a hardware reset.
        if(trial.state().parts[1].voices)
            throw std::runtime_error("Standby left a held/preparing voice active");
        trial.setStandby(false);trial.push(followingNote);render(3200);
        if(!trial.state().parts[1].voices || trial.state().parts[1].volume!=100)
            throw std::runtime_error("Admission standby failed to resume cleanly");
        const uint8_t release[]{0x80,69,0};trial.push(release);render(32000);
        if(trial.state().parts[1].voices)
            throw std::runtime_error("Standby retained sustain after resume");
    }
    std::puts("Native standby: six admission/sustain/stealing boundaries and resume/release PASS");
}

// Exercise the product's semantic commands against physical H8 button input.
// Compare settings, not LCD cancellation alone, without patching H8 RAM.
inline void VerifyPanelSettings(Emulator& emu,const RomsetInfo& roms,bool duringStartup=false,bool soloOnly=false)
{
    const auto& raw=roms.rom_data;
    const auto& r1=raw[size_t(RomLocation::ROM1)];
    const auto& r2=raw[size_t(RomLocation::ROM2)];
    sc55::NativeSynth synth(sc55::ImportSoundData(r1,r2),r1,r2,
        raw[size_t(RomLocation::WAVEROM1)],raw[size_t(RomLocation::WAVEROM2)],raw[size_t(RomLocation::WAVEROM3)]);
    if(duringStartup) {
        std::array<AudioFrame<int32_t>,1> frame{};
        const uint8_t note[]{0x90,60,100};
        synth.push(note);synth.render(frame);
        const bool accepted=synth.adjustSelectedPart(sc55::PartParameter::channel,1);
        std::printf("Panel during startup accepted=%u failed=%u\n",accepted,synth.failed());
        if(!accepted || synth.failed()) throw std::runtime_error("Panel edit during startup killed native engine");
        std::array<AudioFrame<int32_t>,257> block{};
        for(unsigned i=0;i<64;++i) synth.render(block);
        if(synth.failed() || synth.state().parts[1].channel!=1)
            throw std::runtime_error("Deferred channel edit was not applied");
        for(unsigned i=0;i<64;++i)
            if(!synth.adjustSelectedPart(sc55::PartParameter::volume,-1))
                throw std::runtime_error("Panel queue filled prematurely");
        if(synth.adjustSelectedPart(sc55::PartParameter::volume,-1) || synth.failed())
            throw std::runtime_error("Panel backpressure must reject without failing engine");
        synth.selectPart(1); // Queued edits must retain their original target.
        for(unsigned i=0;i<64;++i) synth.render(block);
        if(synth.failed() || synth.state().parts[1].volume!=36 || synth.state().parts[2].volume!=100)
            throw std::runtime_error("Panel commands lost their target or were applied twice");
        if(!synth.adjustSelectedPart(sc55::PartParameter::channel,1))
            throw std::runtime_error("Panel queue did not recover after draining");
        for(unsigned i=0;i<64;++i) synth.render(block);
        if(synth.failed() || synth.state().parts[2].channel!=2)
            throw std::runtime_error("Following panel edit was lost");
        std::puts("Panel startup deferral, target capture, full-queue backpressure and recovery PASS");
        return;
    }
    auto& cpu=emu.GetMCU();
    const auto run=[&](unsigned cycles) {
        const auto end=cpu.cycles+cycles;
        while(cpu.cycles<end) emu.Step();
        std::array<AudioFrame<int32_t>,257> frames{};
        unsigned count=cycles/625;
        while(count) {const auto n=std::min(count,257u);synth.render(std::span(frames).first(n));count-=n;}
        if(synth.failed()) throw std::runtime_error("Panel native render failed");
    };
    run(120000000);
    if(soloOnly) {
        unsigned comparisons=0;
        const auto compare=[&] {
            const auto state=synth.state();
            if(state.soloEnabled!=bool(MCU_Read(cpu,0xcdcc)&8)
                || state.allSelected!=bool(MCU_Read(cpu,0xcdcc)&2)) {
                std::printf("Solo comparison %u: native solo=%u all=%u H8 mode=%02x\n",
                    comparisons,state.soloEnabled,state.allSelected,MCU_Read(cpu,0xcdcc));
                throw std::runtime_error("Solo selection differs from H8 panel");
            }
            for(unsigned part=0;part<16;++part)
                if(state.parts[part].voices!=MCU_Read(cpu,0xa1f0+part))
                    throw std::runtime_error("Solo sounding-part counts differ from H8");
            ++comparisons;
        };
        const auto pressMask=[&](uint32_t mask,auto action) {
            cpu.button_pressed.store(mask);action();run(2000000);
            cpu.button_pressed.store(0);run(2000000);compare();
        };
        const auto notes=[&](uint8_t key) {
            const uint8_t bytes[]{0x90,key,100,0x91,key,100,0x92,key,100};
            emu.PostMIDI(std::vector<uint8_t>(std::begin(bytes),std::end(bytes)));
            synth.push(bytes);run(4000000);compare();
        };
        const uint8_t programs[]{0xc0,80,0xc1,80,0xc2,80};
        emu.PostMIDI(std::vector<uint8_t>(std::begin(programs),std::end(programs)));
        synth.push(programs);run(4000000);
        notes(60);
        constexpr auto soloButtons=(1u<<MCU_BUTTON_INST_ALL)|(1u<<MCU_BUTTON_INST_MUTE);
        pressMask(soloButtons,[&] {synth.toggleSolo();});
        if(!synth.state().parts[1].voices || synth.state().parts[2].voices || synth.state().parts[3].voices)
            throw std::runtime_error("Solo did not stop nonselected sounding parts");
        notes(64);
        pressMask(1u<<MCU_BUTTON_INST_MUTE,[&] {synth.toggleMute();});
        if(synth.state().parts[1].muted) throw std::runtime_error("Mute changed during solo");
        pressMask(1u<<MCU_BUTTON_PART_R,[&] {synth.selectPart(1);});
        notes(67);
        pressMask(soloButtons,[&] {synth.toggleSolo();});
        notes(69);
        // A muted selected part is temporarily audible in solo; leaving solo
        // restores that mute, rather than permanently changing Note Receive.
        pressMask(1u<<MCU_BUTTON_INST_MUTE,[&] {synth.toggleMute();});
        pressMask(soloButtons,[&] {synth.toggleSolo();});
        notes(71);
        if(!synth.state().parts[2].muted || !synth.state().parts[2].voices)
            throw std::runtime_error("Solo failed to override mute without editing it");
        pressMask(soloButtons,[&] {synth.toggleSolo();});
        if(!synth.state().parts[2].muted || synth.state().parts[2].voices)
            throw std::runtime_error("Solo exit did not restore the saved mute policy");
        pressMask(1u<<MCU_BUTTON_INST_ALL,[&] {synth.toggleAll();});
        pressMask(1u<<MCU_BUTTON_INST_MUTE,[&] {synth.toggleMute();});
        pressMask(soloButtons,[&] {synth.toggleSolo();});
        notes(72); // ALL solo overrides both global and per-part mute.
        if(!synth.state().parts[1].voices || !synth.state().parts[2].voices || !synth.state().parts[3].voices)
            throw std::runtime_error("ALL solo did not receive every channel");
        pressMask(1u<<MCU_BUTTON_INST_ALL,[&] {synth.toggleAll();});
        pressMask(soloButtons,[&] {synth.toggleSolo();});
        if(synth.state().parts[2].voices || !synth.state().globalMuted)
            throw std::runtime_error("Solo exit did not restore global mute");
        pressMask(soloButtons,[&] {synth.toggleSolo();});
        notes(74);
        const uint8_t reset[]{0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7};
        emu.PostMIDI(std::vector<uint8_t>(std::begin(reset),std::end(reset)));
        synth.push(reset);run(20000000);compare();
        notes(60);
        pressMask(1u<<MCU_BUTTON_INST_ALL,[&] {synth.toggleAll();});
        const uint8_t gmReset[]{0xf0,0x7e,0x7f,0x09,0x01,0xf7};
        emu.PostMIDI(std::vector<uint8_t>(std::begin(gmReset),std::end(gmReset)));
        synth.push(gmReset);run(20000000);compare();
        notes(64);
        pressMask(soloButtons,[&] {synth.toggleSolo();});
        notes(67); // Reset must not silently discard the saved global mute.
        std::printf("Native solo: %u physical-panel/MIDI comparisons; stop, selection, mute override/restore and GS/GM reset PASS\n",comparisons);
        return;
    }
    unsigned checks=0;
    const auto compare=[&](const char* action) {
        const auto state=synth.state();
        const auto check=[&](unsigned address,unsigned actual) {
            const auto expected=MCU_Read(cpu,address);
            if(actual!=expected) {
                std::printf("Panel %s address=%04x native=%u H8=%u\n",action,address,actual,expected);
                throw std::runtime_error("Panel setting differs from H8");
            }
            ++checks;
        };
        check(0x8002,state.masterVolume); check(0x8005,state.masterKeyShift); check(0x8006,state.masterPan);
        check(0x802d,state.reverbLevel); check(0x8034,state.chorusLevel);
        for(unsigned part=0;part<16;++part) {
            const auto base=0x8048+part*0x70;
            const auto& p=state.parts[part];
            check(base,p.bank);check(base+1,p.program);check(base+4,p.channel);
            check(base+6,p.keyShift);check(base+8,p.volume);check(base+9,p.pan);
            check(base+14,p.chorus);check(base+15,p.reverb);
        }
        if(!state.allSelected) {
            const auto display=state.selectedPart;
            const auto part=display==9 ? 0 : display<9 ? display+1 : display;
            for(unsigned i=0;i<12;++i) check(0xcd18+i,uint8_t(state.parts[part].name[i]));
        }
    };
    const auto press=[&](unsigned button,auto command) {
        cpu.button_pressed.store(1u<<button);command();run(2000000);
        cpu.button_pressed.store(0);run(2000000);
        compare("press");
    };
    constexpr std::array<unsigned,14> buttons{
        MCU_BUTTON_INST_L,MCU_BUTTON_INST_R,MCU_BUTTON_LEVEL_L,MCU_BUTTON_LEVEL_R,
        MCU_BUTTON_PAN_L,MCU_BUTTON_PAN_R,MCU_BUTTON_REVERB_L,MCU_BUTTON_REVERB_R,
        MCU_BUTTON_CHORUS_L,MCU_BUTTON_CHORUS_R,MCU_BUTTON_KEY_SHIFT_L,MCU_BUTTON_KEY_SHIFT_R,
        MCU_BUTTON_MIDI_CH_L,MCU_BUTTON_MIDI_CH_R};
    compare("boot");
    // Scalar continuation follows the ROM table: start-part20 -> reverb30,
    // not the numeric address21. Exercise the complete product MIDI path.
    std::vector<uint8_t> systemRange{0xf0,0x41,0x10,0x42,0x12,0x40,1,0x20,5,2,4,5,87};
    unsigned systemSum=0;
    for(unsigned i=5;i<systemRange.size();++i) systemSum+=systemRange[i];
    systemRange.push_back(uint8_t(-systemSum)&127);systemRange.push_back(0xf7);
    emu.PostMIDI(systemRange);synth.push(systemRange);run(4000000);
    compare("system scalar range");
    std::vector<uint8_t> masterRange{0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7e,23,0};
    unsigned masterSum=0;
    for(unsigned i=5;i<masterRange.size();++i) masterSum+=masterRange[i];
    masterRange.push_back(uint8_t(-masterSum)&127);masterRange.push_back(0xf7);
    emu.PostMIDI(masterRange);synth.push(masterRange);run(20000000);
    compare("master range with reset");
    for(unsigned displayPart=0;displayPart<16;++displayPart) {
        for(unsigned i=0;i<buttons.size();++i)
            press(buttons[i],[&] {synth.adjustSelectedPart(static_cast<sc55::PartParameter>(i/2),i&1 ? 1 : -1);});
        if(displayPart==9) {
            // Traverse all kits in both directions, including the end stops.
            for(int direction:{1,-1}) for(unsigned step=0;step<16;++step)
                press(direction>0 ? MCU_BUTTON_INST_R : MCU_BUTTON_INST_L,[&] {
                    synth.adjustSelectedPart(sc55::PartParameter::program,direction);
                });
            const auto midi=[&](std::span<const uint8_t> bytes) {
                emu.PostMIDI(bytes);
                if(synth.push(bytes)!=bytes.size()) throw std::runtime_error("Panel MIDI queue full");
                run(4000000);compare("MIDI name");
            };
            // A live map edit must reach the snapshot, without recreating UI
            // or looking up the original factory name again.
            const unsigned map=(MCU_Read(cpu,0x804d)&0x20) ? 0 : 1;
            std::vector<uint8_t> packet{0xf0,0x41,0x10,0x42,0x12,0x41,uint8_t(map<<4),0};
            constexpr std::array<uint8_t,12> name{'E','D','I','T','E','D',' ','K','I','T',' ',' '};
            packet.insert(packet.end(),name.begin(),name.end());
            unsigned sum=0;for(unsigned i=5;i<packet.size();++i) sum+=packet[i];
            packet.push_back(uint8_t(-sum)&127);packet.push_back(0xf7);midi(packet);
            if(!std::equal(name.begin(),name.end(),synth.state().parts[0].name.begin()))
                throw std::runtime_error("Live rhythm name missing from snapshot");
            // Compare both a family fallback and a rejected selection; the
            // display latch is not the same as the requested program byte.
            for(uint8_t program:{uint8_t(1),uint8_t(65),uint8_t(0)}) {
                std::printf("Panel name requested program=%u\n",program);std::fflush(stdout);
                const uint8_t bytes[]{0xc9,program};midi(bytes);
            }
        }
        if(displayPart!=15) press(MCU_BUTTON_PART_R,[&] {synth.selectPart(1);});
    }
    press(MCU_BUTTON_INST_ALL,[&] {synth.toggleAll();});
    for(unsigned i=0;i<buttons.size();++i)
        press(buttons[i],[&] {synth.adjustSelectedPart(static_cast<sc55::PartParameter>(i/2),i&1 ? 1 : -1);});
    std::printf("Native panel: 16 parts and ALL, %u setting comparisons PASS\n",checks);
}
