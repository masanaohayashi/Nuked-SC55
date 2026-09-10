#pragma once
#include "mcu_interrupt.h"
#include "sc55_synth.h"
#include <functional>

inline std::function<void(uint32_t)> panelBulkInstruction;
enum class PanelBulkSelection { defaults, melodicOnly, none, bothMaps, secondMapOnly };

// Observe the real panel producer, without injecting PC, RAM or internal MIDI.
inline void VerifyPanelBulkSequence(Emulator& emu,const RomsetInfo& roms,
    sc55::BulkReplyTransfer::PanelScope scope=sc55::BulkReplyTransfer::PanelScope::allSettings,
    PanelBulkSelection selectionCase=PanelBulkSelection::defaults)
{
    using Scope=sc55::BulkReplyTransfer::PanelScope;
    const unsigned selectedCount=selectionCase==PanelBulkSelection::none ? 0 : selectionCase==PanelBulkSelection::melodicOnly ? 15 : 16;
    // Partial dumps with both maps lose map1 after map0's parser call.
    const unsigned maps=selectionCase==PanelBulkSelection::melodicOnly || selectionCase==PanelBulkSelection::none ? 0 : 1;
    const unsigned systemCount=scope==Scope::systemAndParts ? 2 : 0;
    const unsigned expectedRequests=scope==Scope::allSettings ? 17 : systemCount+selectedCount+maps*8;
    const unsigned expectedCount=scope==Scope::allSettings ? 60 : systemCount+selectedCount*2+maps*15;
    auto& cpu=emu.GetMCU();
    unsigned starts=0,finishes=0,requests=0,packets=0,producerWaits=0;
    bool observing=false;
    std::vector<uint8_t> wire;
    std::vector<std::vector<uint8_t>> expectedPackets;
    panelBulkInstruction=[&](uint32_t pc) {
            if(observing) {
                if(pc==0x04719b)
                    std::printf("Panel selection part=%u flags=%02x accumulated=%02x\n",cpu.r[0],
                        MCU_Read(cpu,cpu.r[2]),cpu.r[5]);
                if(pc==0x047318 || pc==0x0471ef)
                    std::printf("Panel parser pc=%06x task=%u r5=%04x opcode=%02x\n",pc,
                        MCU_Read16(cpu,0xfdca),cpu.r[5],MCU_Read(cpu,pc));
                if(pc==0x047318 && (MCU_Read(cpu,pc)!=3 || MCU_Read16(cpu,0xfdca)!=7))
                    throw std::runtime_error("Panel request was not a direct parser call in task7");
                if(pc==0x0471ef && selectionCase==PanelBulkSelection::bothMaps && cpu.r[5]!=0)
                    throw std::runtime_error("H8 partial dump map selection no longer matches observed clobber");
                if(pc==0x041d6c) ++starts;
                if(pc==0x0428c2) ++finishes;
                if(pc==0x0470e8 || pc==0x04712a) {
                    if(cpu.r[0]!=40 || producerWaits!=requests || !wire.empty())
                        throw std::runtime_error("Panel producer wait overlaps unfinished request");
                    ++producerWaits;
                }
                if(pc==0x0470e8 || pc==0x04712a || pc==0x047320 || pc==0x047154 || pc==0x04424b
                    || pc==0x0426ef)
                    std::printf("Panel bulk transition pc=%06x cycles=%llu argument=%u packets=%u RX=%02x\n",
                        pc,(unsigned long long)cpu.cycles,cpu.r[0],packets,cpu.dev_register[DEV_SCR]);
                if(pc==0x047300) {
                    ++requests;
                    std::printf("Panel bulk request %u cycles=%llu RX=%02x outer=%02x input=%04x/%04x payload=",
                        requests,(unsigned long long)cpu.cycles,cpu.dev_register[DEV_SCR],
                        MCU_Read(cpu,0xcf02),MCU_Read16(cpu,0xabfe),MCU_Read16(cpu,0xac00));
                    for(unsigned i=0;i<6;++i) std::printf("%02x",MCU_Read(cpu,0xcf7b+i));
                    std::puts("");
                }
                if(pc==0x041c78 || pc==0x041d2b) {
                    if(wire.size()<10 || wire.front()!=0xf0 || wire.back()!=0xf7)
                        throw std::runtime_error("Panel bulk TX packet incomplete");
                    unsigned sum=0;
                    for(unsigned i=5;i+1<wire.size();++i) sum+=wire[i];
                    if(sum&127) throw std::runtime_error("Panel bulk TX checksum invalid");
                    ++packets;
                    if(packets==15) cpu.button_pressed.store(1u<<MCU_BUTTON_LEVEL_R);
                    if(packets==16) cpu.button_pressed.store(0);
                    std::printf("Panel bulk packet %u cycles=%llu address=%02x%02x%02x bytes=%zu\n",
                        packets,(unsigned long long)cpu.cycles,wire[5],wire[6],wire[7],wire.size());
                    expectedPackets.push_back(wire);wire.clear();
                }
            }
    };
    struct ClearObserver { ~ClearObserver() { panelBulkInstruction={}; } } clearObserver;
    const auto run=[&](uint64_t cycles) {
        const auto end=cpu.cycles+cycles;
        while(cpu.cycles<end) {
            if((cpu.dev_register[DEV_SCR]&0xa0)==0xa0 && (cpu.dev_register[DEV_SSR]&0x80))
                MCU_Interrupt_SetRequest(cpu,INTERRUPT_SOURCE_UART_TX,1);
            const auto before=MCU_Read16(cpu,0xabfc);
            emu.Step();
            const auto after=MCU_Read16(cpu,0xabfc);
            const auto count=(after-before)&255;
            if(observing && before<256 && after<256 && count<=2)
                for(unsigned i=0;i<count;++i) wire.push_back(MCU_Read(cpu,0xa5f8+((before+i)&255)));
        }
    };
    run(120000000);
    std::vector<uint8_t> settings;
    const auto dt1=[&](uint8_t part,uint8_t address,uint8_t value) {
        const uint8_t bytes[]{0xf0,0x41,0x10,0x42,0x12,0x40,uint8_t(0x10+part),address,value,
            uint8_t(-(0x50+part+address+value)&127),0xf7};
        settings.insert(settings.end(),std::begin(bytes),std::end(bytes));
    };
    if(selectionCase==PanelBulkSelection::melodicOnly) dt1(0,8,0);
    if(selectionCase==PanelBulkSelection::none) for(unsigned part=0;part<16;++part) dt1(uint8_t(part),8,0);
    if(selectionCase==PanelBulkSelection::bothMaps) dt1(2,0x15,2);
    if(selectionCase==PanelBulkSelection::secondMapOnly) dt1(0,0x15,2);
    if(!settings.empty()) {
        emu.PostMIDI(settings);run(12000000);
        std::printf("Panel fixture rhythm flags=%02x/%02x\n",MCU_Read(cpu,0x804d),MCU_Read(cpu,0x812d));
    }
    const auto press=[&](uint32_t buttons) {
        cpu.button_pressed.store(buttons);run(2000000);
        cpu.button_pressed.store(0);run(2000000);
        std::printf("Panel mode=%u page=%u confirm=%02x selection=%u\n",MCU_Read(cpu,0xcdca),
            MCU_Read(cpu,0xcdcb),MCU_Read(cpu,0xcdcc),MCU_Read(cpu,0xce5a));
    };
    // Page/display selection chooses713e,715f or717d; the same physical
    // INSTRUMENT pair enters confirmation and ALL executes the chosen scope.
    if(scope!=Scope::parts) press(1u<<MCU_BUTTON_INST_ALL);
    if(scope!=Scope::allSettings) press((1u<<MCU_BUTTON_PART_L)|(1u<<MCU_BUTTON_PART_R));
    press((1u<<MCU_BUTTON_INST_L)|(1u<<MCU_BUTTON_INST_R));
    const unsigned selection=scope==Scope::allSettings ? 0 : scope==Scope::systemAndParts ? 1 : 2;
    if(!(MCU_Read(cpu,0xcdcc)&128) || MCU_Read(cpu,0xce5a)!=selection)
        throw std::runtime_error("Physical panel did not select all-data bulk confirmation");
    observing=true;
    press(1u<<MCU_BUTTON_INST_ALL);
    run(240000000);
    std::printf("Panel bulk sequence: starts=%u requests=%u packets=%u finishes=%u outer=%u\n",
        starts,requests,packets,finishes,MCU_Read(cpu,0xcf02)&1);
    if(starts!=1 || requests!=expectedRequests || producerWaits!=expectedRequests || packets!=expectedCount
        || finishes!=1 || (MCU_Read(cpu,0xcf02)&1)
        || MCU_Read(cpu,0x8002)!=127 || MCU_Read(cpu,0x80c0)!=100)
        throw std::runtime_error("Panel bulk sequence incomplete or unexpected");

    const auto& raw=roms.rom_data;
    const auto& r1=raw[size_t(RomLocation::ROM1)];const auto& r2=raw[size_t(RomLocation::ROM2)];
    sc55::NativeSynth synth(sc55::ImportSoundData(r1,r2),r1,r2,
        raw[size_t(RomLocation::WAVEROM1)],raw[size_t(RomLocation::WAVEROM2)],raw[size_t(RomLocation::WAVEROM3)]);
    std::array<AudioFrame<int32_t>,1> frame{};
    if(!settings.empty()) {
        synth.push(settings);
        for(unsigned i=0;i<19200;++i) synth.render(frame);
    }
    if(synth.requestSettingsDump(scope)) throw std::runtime_error("Unconnected panel dump accepted");
    if(!synth.setBulkOutputConnected(true) || !synth.requestSettingsDump(scope))
        throw std::runtime_error("Native panel dump rejected");
    if(expectedCount && (synth.requestAllSettingsDump() || synth.setBulkOutputConnected(false)))
        throw std::runtime_error("Active panel transfer replaced or disconnected");
    sc55::BulkReplyTransfer::Packet packet;
    unsigned compared=0;
    for(unsigned sample=0;sample<160000;++sample) {
        synth.render(frame);
        if(synth.takeBulkReply(packet)) {
            if(compared>=expectedPackets.size() || packet.size!=expectedPackets[compared].size()
                || !std::equal(expectedPackets[compared].begin(),expectedPackets[compared].end(),packet.bytes.begin()))
                throw std::runtime_error("Native all-settings packet differs from physical H8 panel dump");
            ++compared;
            if(!synth.adjustSelectedPart(sc55::PartParameter::volume,-1))
                throw std::runtime_error("Panel fixture edit queue unexpectedly full");
            // During both system and drum phases RX remains disabled.
            const uint8_t during[]{0xb0,7,17};synth.push(during);
            // Rendering alone never acknowledges transmission.
            for(unsigned i=0;i<1500;++i) synth.render(frame);
            if(synth.takeBulkReply(packet) || !synth.completeBulkReplyTransmission()
                || synth.completeBulkReplyTransmission())
                throw std::runtime_error("Native panel TX completion contract violated");
        }
    }
    if(compared!=expectedCount || synth.failed() || synth.state().parts[1].volume!=100
        || !synth.setBulkOutputConnected(false))
        throw std::runtime_error("Native all-settings transfer did not finish with RX isolation");
    const uint8_t following[]{0xb0,7,23};synth.push(following);
    for(unsigned i=0;i<1600;++i) synth.render(frame);
    if(synth.state().parts[1].volume!=23) throw std::runtime_error("Native panel dump did not resume MIDI");
    std::printf("Native panel dump scope=%u: %u packets match physical H8 panel, TX wait and RX isolation PASS\n",
        unsigned(scope),compared);
}
