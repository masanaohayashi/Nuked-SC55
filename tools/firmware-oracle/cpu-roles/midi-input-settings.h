#pragma once
#include "sc55_synth.h"
#include "NativeMidiInputState.h"
#include <juce_core/juce_core.h>

// Exploration through actual switches. Read-only firmware state observations;
// no PC/RAM patching and no native behavior inferred from a guessed key chord.
inline void InspectMidiInputPanel(Emulator& emu)
{
    auto& cpu=emu.GetMCU();
    const auto run=[&](uint32_t mask,uint64_t cycles,const char* label) {
        cpu.button_pressed.store(mask);
        const auto end=cpu.cycles+cycles;while(cpu.cycles<end) emu.Step();
        std::printf("Input panel %s mask=%08x page=%u mode=%u item=%u reset=%u exclusive=%u options=%04x\n",
            label,mask,MCU_Read(cpu,0xcdca),MCU_Read(cpu,0xcdcb),MCU_Read(cpu,0xcf37),
            MCU_Read(cpu,0xcdf7),MCU_Read(cpu,0xcdf8),MCU_Read16(cpu,0xcdc8));
    };
    const uint32_t all=1u<<MCU_BUTTON_INST_ALL,mute=1u<<MCU_BUTTON_INST_MUTE;
    run(0,120000000,"boot");
    run(all,2000000,"ALL held");run(0,2000000,"ALL release");
    const uint32_t parts=(1u<<MCU_BUTTON_PART_L)|(1u<<MCU_BUTTON_PART_R);
    run(parts,2000000,"PART L+R");run(0,2000000,"PART release");
    for(unsigned i=0;i<6;++i) {
        run(mute,2000000,"next setting");run(0,2000000,"MUTE release");
        if(i==3 || i==4) {
            run(1u<<MCU_BUTTON_INST_L,2000000,"setting decrement");
            run(0,2000000,"INST release");
        }
    }
    run(0,2000000,"release");
}

inline void VerifyMidiInputSettings(Emulator& emu,const RomsetInfo& roms)
{
    NativeMidiInputState saved;
    for(unsigned id=0;id<32;++id) for(unsigned flags=0;flags<16;++flags) {
        const sc55::MidiInputSettings input{uint8_t(id),bool(flags&1),bool(flags&2),bool(flags&4),bool(flags&8)};
        const auto encoded=NativeMidiInputState::encode(input);
        juce::XmlElement xml("SC55");xml.setAttribute("nativeMidiInputV1",int(encoded));
        const auto parsedXml=juce::XmlDocument::parse(xml.toString());
        const auto decoded=NativeMidiInputState::parse(parsedXml->getStringAttribute("nativeMidiInputV1").toStdString());
        if(!decoded || *decoded!=encoded) throw std::runtime_error("MIDI input XML roundtrip failed");
        saved.restore(*decoded);
        // A publication from the old audio state must not overwrite restore.
        saved.publish(sc55::MidiInputSettings{});
        if(saved.encoded()!=encoded) throw std::runtime_error("Audio publication lost pending restore");
        const auto pending=saved.takeRestore();
        if(!pending || NativeMidiInputState::encode(*pending)!=encoded || saved.takeRestore())
            throw std::runtime_error("MIDI input restore handoff failed");
        saved.publish(*pending);
        if(NativeMidiInputState::encode(saved.read())!=encoded)
            throw std::runtime_error("Applied MIDI input state was not saved");
    }
    for(const auto invalid:{"", "-1", "32", "1824", "4294967296", "12x", "1.5", " 16"})
        if(NativeMidiInputState::parse(invalid)) throw std::runtime_error("Invalid MIDI input state was accepted");
    saved.restore(-1);
    if(saved.encoded()!=NativeMidiInputState::defaultValue)
        throw std::runtime_error("Invalid/legacy MIDI input state did not restore defaults");
    if(!NativeMidiInputState::decode(0x310)->receiveProgramChanges)
        throw std::runtime_error("Legacy session disabled Program Change reception");
    std::puts("MIDI input state: 512 XML roundtrips, legacy defaults, restore/publication ordering and invalid values PASS");
    auto& cpu=emu.GetMCU();
    const auto boot=cpu.cycles+120000000;
    while(cpu.cycles<boot) emu.Step();
    const auto resetVolume=MCU_Read(cpu,0x8002);
    const auto runH8=[&](uint64_t cycles) {
        const auto end=cpu.cycles+cycles;while(cpu.cycles<end) emu.Step();
    };
    const auto pressH8=[&](unsigned button) {
        cpu.button_pressed.store(1u<<button);runH8(2000000);
        cpu.button_pressed.store(0);runH8(2000000);
    };
    pressH8(MCU_BUTTON_INST_ALL);
    pressH8(MCU_BUTTON_MIDI_CH_R);
    std::fprintf(stderr,"H8 ALL/channel+: page=%u id=%u\n",MCU_Read(cpu,0xcdca),MCU_Read(cpu,0xac24));
    if(MCU_Read(cpu,0xac24)!=0x11) throw std::runtime_error("ALL MIDI channel did not select device ID");
    pressH8(MCU_BUTTON_MIDI_CH_L);pressH8(MCU_BUTTON_INST_ALL);
    const sc55::MidiInputSettings defaults;
    if(defaults.deviceId!=MCU_Read(cpu,0xac24)
        || defaults.receiveExclusive!=bool(MCU_Read(cpu,0xcdf8))
        || defaults.receiveReset!=bool(MCU_Read(cpu,0xcdf7))
        || defaults.receiveProgramChanges!=bool(MCU_Read(cpu,0xcdf6))
        || defaults.ignoreChecksum!=bool(MCU_Read16(cpu,0xcdc8)&0x200))
        throw std::runtime_error("Native device input defaults differ from H8 boot");
    const auto& raw=roms.rom_data;
    const auto& r1=raw[size_t(RomLocation::ROM1)];const auto& r2=raw[size_t(RomLocation::ROM2)];
    sc55::NativeSynth synth(sc55::ImportSoundData(r1,r2),r1,r2,
        raw[size_t(RomLocation::WAVEROM1)],raw[size_t(RomLocation::WAVEROM2)],raw[size_t(RomLocation::WAVEROM3)]);
    const auto render=[&] {
        std::array<AudioFrame<int32_t>,257> frames;
        for(unsigned i=0;i<128;++i) synth.render(frames);
        if(synth.failed()) throw std::runtime_error("Input settings caused native failure");
    };
    const auto dt1=[&](std::initializer_list<uint8_t> payload,uint8_t device=0x10,bool corrupt=false,uint8_t model=0x42) {
        std::vector<uint8_t> packet{0xf0,0x41,device,model,0x12};
        unsigned sum=0;for(auto b:payload) {packet.push_back(b);sum+=b;}
        packet.push_back(uint8_t(-sum+(corrupt ? 1 : 0))&127);packet.push_back(0xf7);
        for(auto byte:packet) if(synth.push(std::span(&byte,1))!=1)
            throw std::runtime_error("Fragmented settings input rejected");
        render();
    };
    const auto volume=[&](unsigned expected) {
        const auto actual=synth.state().masterVolume;
        if(actual!=expected) {
            std::fprintf(stderr,"Master volume actual=%u expected=%u\n",actual,expected);
            throw std::runtime_error("Device input gate changed the wrong setting");
        }
    };
    synth.toggleAll();pressH8(MCU_BUTTON_INST_ALL);
    for(const int direction:{1,-1}) {
        for(unsigned i=0;i<40;++i) {
            synth.adjustSelectedPart(sc55::PartParameter::channel,direction);render();
            pressH8(direction>0 ? MCU_BUTTON_MIDI_CH_R : MCU_BUTTON_MIDI_CH_L);
            if(synth.state().midiInput.deviceId!=MCU_Read(cpu,0xac24))
                throw std::runtime_error("ALL MIDI channel device-ID stepping/clamp differs from H8");
        }
    }
    // After both clamps, return to17 (wire10) using the same physical edits.
    for(unsigned i=0;i<16;++i) {
        synth.adjustSelectedPart(sc55::PartParameter::channel,1);render();
        pressH8(MCU_BUTTON_MIDI_CH_R);
    }
    synth.adjustSelectedPart(sc55::PartParameter::channel,1);render();pressH8(MCU_BUTTON_MIDI_CH_R);
    const uint8_t wrongDevice[]{0xf0,0x41,0x10,0x42,0x12,0x40,0,4,37,23,0xf7};
    const uint8_t rightDevice[]{0xf0,0x41,0x11,0x42,0x12,0x40,0,4,37,23,0xf7};
    for(const auto& packet:{wrongDevice,rightDevice}) {
        // The arrays decay in this initializer list; each packet is11 bytes.
        emu.PostMIDI(std::span(packet,11));synth.push(std::span(packet,11));runH8(4000000);render();
        const auto expected=packet[2]==0x11 ? 37 : resetVolume;
        if(synth.state().masterVolume!=expected || MCU_Read(cpu,0x8002)!=expected)
            throw std::runtime_error("Panel-selected device ID changed MIDI acceptance differently from H8");
    }
    synth.adjustSelectedPart(sc55::PartParameter::channel,-1);render();pressH8(MCU_BUTTON_MIDI_CH_L);
    synth.toggleAll();pressH8(MCU_BUTTON_INST_ALL);
    std::puts("ALL MIDI channel: H8 physical buttons/native edits, both clamps and wrong/right-device DT1 acceptance PASS");
    const uint8_t gm[]{0xf0,0x7e,0x7f,9,1,0xf7};
    {
        const auto both=[&](std::span<const uint8_t> packet) {
            emu.PostMIDI(packet);synth.push(packet);runH8(4000000);render();
            if(synth.state().masterVolume!=MCU_Read(cpu,0x8002))
                throw std::runtime_error("Physical H8 input gate differs from native MIDI reception");
        };
        const auto bothDt1=[&](std::initializer_list<uint8_t> payload) {
            std::vector<uint8_t> packet{0xf0,0x41,0x10,0x42,0x12};
            unsigned sum=0;for(auto byte:payload) {packet.push_back(byte);sum+=byte;}
            packet.push_back(uint8_t(-sum)&127);packet.push_back(0xf7);both(packet);
        };
        const auto partsChord=[&] {
            cpu.button_pressed.store((1u<<MCU_BUTTON_PART_L)|(1u<<MCU_BUTTON_PART_R));
            runH8(2000000);cpu.button_pressed.store(0);runH8(2000000);
        };
        // ROM27b0 maps the two PART switches to event19. ALL screen +
        // event19 selects system settings, MUTE advances its item, INST edits.
        pressH8(MCU_BUTTON_INST_ALL);partsChord();
        for(unsigned i=0;i<3;++i) pressH8(MCU_BUTTON_INST_MUTE);
        pressH8(MCU_BUTTON_INST_L);
        if(MCU_Read(cpu,0xcf37)!=3 || MCU_Read(cpu,0xcdf6)!=0)
            throw std::runtime_error("Physical panel did not disable instrument change reception");
        auto input=defaults;input.receiveProgramChanges=false;synth.setMidiInputSettings(input);
        const auto programs=[&](unsigned melodic,unsigned drum) {
            if(synth.state().parts[1].program!=melodic || MCU_Read(cpu,0x80b9)!=melodic
                || synth.state().parts[0].program!=drum || MCU_Read(cpu,0x8049)!=drum)
                throw std::runtime_error("Device-level Program Change gate differs from H8");
        };
        const uint8_t programChanges[]{0xc0,12,0xc9,8};
        both(programChanges);programs(0,0);
        // Global MIDI PC rejection must not suppress explicit GS tone writes.
        bothDt1({0x40,0x11,0,0,16});programs(16,0);
        both(programChanges);programs(16,0);
        pressH8(MCU_BUTTON_INST_R);
        input.receiveProgramChanges=true;synth.setMidiInputSettings(input);
        both(programChanges);programs(12,8);
        pressH8(MCU_BUTTON_INST_MUTE);
        pressH8(MCU_BUTTON_INST_L);
        if(MCU_Read(cpu,0xcdca)!=2 || MCU_Read(cpu,0xcdcb)!=1
            || MCU_Read(cpu,0xcf37)!=4 || MCU_Read(cpu,0xcdf7)!=0)
            throw std::runtime_error("Physical panel did not disable reset reception");
        input.receiveReset=false;synth.setMidiInputSettings(input);
        bothDt1({0x40,0,4,37});both(gm);volume(37);
        bothDt1({0x40,0,0x7f,0});volume(37);
        bothDt1({0x40,0,4,53,64,64,84,0});volume(53);
        pressH8(MCU_BUTTON_INST_MUTE);pressH8(MCU_BUTTON_INST_L);
        if(MCU_Read(cpu,0xcf37)!=5 || MCU_Read(cpu,0xcdf8)!=0)
            throw std::runtime_error("Physical panel did not disable exclusive reception");
        input.receiveExclusive=false;synth.setMidiInputSettings(input);
        bothDt1({0x40,0,4,61});both(gm);volume(53);
        const uint8_t control[]{0xb0,7,27};both(control);
        if(synth.state().parts[1].volume!=27 || MCU_Read(cpu,0x80c0)!=27)
            throw std::runtime_error("Exclusive gate blocked ordinary CC differently from H8");
        pressH8(MCU_BUTTON_INST_R);
        input.receiveExclusive=true;synth.setMidiInputSettings(input);
        bothDt1({0x40,0,4,61});volume(61);
        pressH8(MCU_BUTTON_INST_ALL);pressH8(MCU_BUTTON_INST_R);
        if(MCU_Read(cpu,0xcdf7)!=1 || MCU_Read(cpu,0xcdf8)!=1)
            throw std::runtime_error("Physical input settings did not restore");
        synth.setMidiInputSettings(defaults);both(gm);volume(resetVolume);
        partsChord();pressH8(MCU_BUTTON_INST_ALL);
        std::puts("H8 physical settings: instrument/reset/exclusive off/on, melodic/drum PC gates, DT1 bypass, GS+GM gates, prefix writes and ordinary CC match native PASS");
    }
    dt1({0x40,0,4,37});volume(37);
    auto settings=defaults;settings.receiveExclusive=false;synth.setMidiInputSettings(settings);
    dt1({0x40,0,4,44});synth.push(gm);render();volume(37);
    const auto beforeDisplay=synth.state().displayEvents.sequence;
    dt1({0x10,0,0,'X'},0x10,false,0x45);
    if(synth.state().displayEvents.sequence!=beforeDisplay)
        throw std::runtime_error("Disabled exclusive reception admitted display data");
    const uint8_t cc[]{0xb0,7,27};synth.push(cc);render();
    if(synth.state().parts[1].volume!=27) throw std::runtime_error("Exclusive gate blocked ordinary MIDI");
    settings.receiveExclusive=true;settings.receiveReset=false;synth.setMidiInputSettings(settings);
    synth.push(gm);render();dt1({0x40,0,0x7f,0});volume(37);
    // A reset-disabled multi-record DT1 must still commit preceding controls.
    dt1({0x40,0,4,53,64,64,84,0});volume(53);
    settings.deviceId=0x11;synth.setMidiInputSettings(settings);
    dt1({0x40,0,4,61});volume(53);
    dt1({0x40,0,4,61},0x11);volume(61);
    dt1({0x40,0,4,72},0x11,true);volume(61);
    settings.ignoreChecksum=true;synth.setMidiInputSettings(settings);
    dt1({0x40,0,4,72},0x11,true);volume(72);
    settings.receiveReset=true;synth.setMidiInputSettings(settings);
    synth.push(gm);render();volume(resetVolume);
    if(synth.midiInputSettings().deviceId!=0x11 || !synth.midiInputSettings().ignoreChecksum)
        throw std::runtime_error("GS defaults overwrote device-level input settings");
    dt1({0x40,0,4,81},0x11);volume(81);
    dt1({0x40,0,0x7f,0},0x11);volume(resetVolume);
    std::puts("MIDI input defaults match H8 boot. Native input-path checks PASS: exclusive/reset gates, ordinary CC, prefix writes, device ID, checksum, reset persistence.");
    std::puts("Checksum-override physical panel entry remains unverified; native input-path coverage is separate.");
}
