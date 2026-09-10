#pragma once
#include "sc55_synth.h"

// Exercise the product's semantic commands against physical H8 button input.
// Compare settings, not LCD cancellation alone, without patching H8 RAM.
inline void VerifyPanelSettings(Emulator& emu,const RomsetInfo& roms,bool duringStartup=false)
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
