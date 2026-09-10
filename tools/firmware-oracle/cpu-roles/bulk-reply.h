#pragma once
#include "sc55_bulk_reply.h"
#include "mcu_interrupt.h"
#include "sc55_native_player.h"

// Native receive/control/transfer oracle. A diagnostic transport takes packets
// and acknowledges completion; the plugin's host output remains unconnected.
inline void VerifyBulkReplies(Emulator& emu,const RomsetInfo& roms)
{
    const auto& raw=roms.rom_data;
    const auto& r1=raw[size_t(RomLocation::ROM1)];
    const auto& r2=raw[size_t(RomLocation::ROM2)];
    sc55::SoundData sounds;
    if(!sounds.loadEncoded(sc55::ImportSoundData(r1,r2))) throw std::runtime_error("Invalid sound data");
    auto pcm=std::make_unique<pcm_t>(); pcm->is_mk1=true;
    std::copy_n(emu.GetPCM().waverom1,sizeof(pcm->waverom1),pcm->waverom1);
    std::copy_n(emu.GetPCM().waverom2,sizeof(pcm->waverom2),pcm->waverom2);
    std::copy_n(emu.GetPCM().waverom3,sizeof(pcm->waverom3),pcm->waverom3);
    sc55::NativeMelodicPlayer player(sounds,*pcm,sc55::ImportSystemDefaults(r1,r2),
        sc55::ImportRhythmPresets(r1,r2),sc55::ImportMelodicPresets(r1,r2),sc55::ImportEffectsTables(r1,r2));
    player.renderFrames(16384);
    auto& cpu=emu.GetMCU();
    const auto boot=cpu.cycles+120000000;
    while(cpu.cycles<boot) emu.Step();
    player.setBulkOutputConnected(true);
    const uint8_t note[]{0xb0,11,37,0x90,60,100};
    emu.PostMIDI(note); player.push(note);
    const auto noteEnd=cpu.cycles+2000000;
    while(cpu.cycles<noteEnd) emu.Step();
    player.renderFrames(3200);
    if(player.freeVoices()==24 || MCU_Read(cpu,0xa3c1)==24)
        throw std::runtime_error("Bulk fixture did not start a voice");
    unsigned packets=0,requests=0;
    const auto run=[&](std::array<uint8_t,6> payload) {
        sc55::BulkReplyTransfer transfer;
        if(transfer.begin(payload)!=sc55::BulkReplyTransfer::BeginResult::accepted)
            throw std::runtime_error("Native bulk rejected valid request");
        if(transfer.begin(payload)!=sc55::BulkReplyTransfer::BeginResult::busy)
            throw std::runtime_error("Native bulk replaced active transaction");
        std::vector<uint8_t> input{0xf0,0x41,0x10,0x42,0x11}; unsigned sum=0;
        for(auto b:payload) {input.push_back(b);sum+=b;}
        input.push_back(uint8_t(-sum)&127); input.push_back(0xf7); emu.PostMIDI(input);
        const auto oldVolume=player.partSettings().parts[1].controls.volume;
        const uint8_t following[]{0xb0,7,uint8_t(oldVolume==93 ? 94 : 93)};
        emu.PostMIDI(following); player.push(input); player.push(following);
        player.renderFrames(6400);
        if(!player.bulkReplyPending() || player.partSettings().parts[1].controls.volume!=oldVolume
            || player.freeVoices()!=24 || player.partSettings().parts[1].controls.expression!=127)
            throw std::runtime_error("Bulk begin did not stop voices/reset controllers/hold following MIDI");
        if(player.setBulkOutputConnected(false)) throw std::runtime_error("Disconnected during active transfer");
        sc55::BulkReplyTransfer::Packet expected;
        std::vector<uint8_t> actual;
        bool havePacket=false;
        bool injectedDuring=false;
        const auto oldPan=player.partSettings().parts[1].controls.pan;
        const uint8_t during[]{0xb0,10,uint8_t(oldPan==51 ? 52 : 51)};
        const auto load=[&](uint8_t region,unsigned map,unsigned offset) {
            if(region==0x49) return player.rhythmRecords().at(map).at(offset);
            const auto value=player.readSystemConfiguration(offset);
            if(!value) throw std::runtime_error("Missing native setting for bulk readback");
            return *value;
        };
        const auto end=cpu.cycles+80000000;
        while(cpu.cycles<end) {
            if((cpu.dev_register[DEV_SCR]&0xa0)==0xa0 && (cpu.dev_register[DEV_SSR]&0x80))
                MCU_Interrupt_SetRequest(cpu,INTERRUPT_SOURCE_UART_TX,1);
            const unsigned pc=(unsigned(cpu.cp)<<16)|cpu.pc;
            if(pc==0x041c54 || pc==0x041cc0) {
                if(havePacket || !transfer.next(expected,load))
                    throw std::runtime_error("Native bulk packet schedule differs from H8");
                sc55::BulkReplyTransfer::Packet native;
                if(!player.takeBulkReply(native) || native.size!=expected.size
                    || !std::equal(native.bytes.begin(),native.bytes.begin()+native.size,expected.bytes.begin()))
                    throw std::runtime_error("Native MIDI bulk request did not emit expected packet");
                player.renderFrames(3200); // PCM advances, but TX is not yet acknowledged.
                if(player.takeBulkReply(native) || !player.bulkReplyPending()
                    || player.partSettings().parts[1].controls.volume!=oldVolume)
                    throw std::runtime_error("Native bulk advanced before transport completion");
                havePacket=true; actual.clear();
                if(!injectedDuring) { emu.PostMIDI(during); player.push(during); injectedDuring=true; }
                sc55::BulkReplyTransfer::Packet ignored;
                transfer.advanceKernelTicks(1000);
                if(transfer.next(ignored,load)) throw std::runtime_error("Bulk advanced before TX completion");
            }
            if(pc==0x041c78 || pc==0x041d2b) {
                if(!havePacket || actual.size()!=expected.size
                    || !std::equal(actual.begin(),actual.end(),expected.bytes.begin()))
                    throw std::runtime_error("Bulk packet differs from H8 TX");
                if(cpu.r[0]!=MCU_Read16(cpu,0xabfc))
                    throw std::runtime_error("H8 began spacing before TX drain");
                if(!transfer.transmissionComplete() || transfer.transmissionComplete())
                    throw std::runtime_error("Bulk TX completion state invalid");
                if(!player.completeBulkReplyTransmission() || player.completeBulkReplyTransmission())
                    throw std::runtime_error("Native transport completion state invalid");
                player.renderFrames(1600); // Beyond the actual40 kernel tick spacing.
                ++packets; havePacket=false;
            }
            // Observe H8's actual wait request, then test both sides of that
            // tick boundary. This proves relative ticks, not CPU-cycle parity.
            if(pc==0x041c7b || pc==0x041d2e) {
                if(cpu.r[0]!=40) throw std::runtime_error("H8 bulk wait is not40 ticks");
                transfer.advanceKernelTicks(39);
                sc55::BulkReplyTransfer::Packet ignored;
                if(!transfer.active() || transfer.next(ignored,load))
                    throw std::runtime_error("Bulk spacing ended before40 ticks");
                transfer.advanceKernelTicks(1);
            }
            const auto before=MCU_Read16(cpu,0xabfc);
            emu.Step();
            const auto after=MCU_Read16(cpu,0xabfc);
            const unsigned count=(after-before)&255;
            if(havePacket && before<256 && after<256 && count<=2)
                for(unsigned i=0;i<count;++i) actual.push_back(MCU_Read(cpu,0xa5f8+((before+i)&255)));
        }
        if(transfer.active() || havePacket) throw std::runtime_error("Bulk transfer did not finish");
        if(MCU_Read(cpu,0x80c1)!=oldPan || player.partSettings().parts[1].controls.pan!=oldPan)
            throw std::runtime_error("Transfer-time MIDI was not discarded like H8");
        if(player.bulkReplyPending() || player.failed() || player.queuedEvents()!=0
            || player.partSettings().parts[1].controls.volume!=oldVolume
            || MCU_Read(cpu,0x80c0)!=oldVolume || MCU_Read(cpu,0xa3c1)!=24
            || MCU_Read(cpu,0xab37)!=127)
            throw std::runtime_error("Bulk receive-disable behavior differs from H8");
        // New MIDI after transfer completion must work; the pre-transfer tail
        // above was discarded by04:2715, not saved for delayed execution.
        emu.PostMIDI(following); player.push(following);
        const auto resumedEnd=cpu.cycles+2000000;
        while(cpu.cycles<resumedEnd) emu.Step();
        player.renderFrames(3200);
        if(player.partSettings().parts[1].controls.volume!=following[2] || MCU_Read(cpu,0x80c0)!=following[2])
            throw std::runtime_error("Bulk end did not resume new MIDI like H8");
        ++requests;
    };
    run({0x48,0,0,0,29,16}); // Entire748h settings region, including final8bytes.
    run({0x48,0,3,0,1,2}); // Odd wire address floors; crosses a64-byte boundary.
    for(unsigned map=0;map<2;++map)
        for(unsigned block=0;block<=14;block+=2)
            run({0x49,uint8_t(map*16+block),0,0,uint8_t(block==14 ? 0 : 2),uint8_t(block==14 ? 24 : 0)});
    for(auto payload:std::array<std::array<uint8_t,6>,6>{{
        {0x48,0,0,0,0,0},{0x48,29,16,0,0,2},{0x48,0,0,1,0,2},
        {0x49,1,0,0,2,0},{0x49,2,1,0,2,0},{0x49,14,0,0,2,0}}}) {
        sc55::BulkReplyTransfer transfer;
        if(transfer.begin(payload)!=sc55::BulkReplyTransfer::BeginResult::invalid || transfer.active())
            throw std::runtime_error("Native bulk accepted invalid transaction");
    }
    if(!player.setBulkOutputConnected(false)) throw std::runtime_error("Could not disconnect idle bulk output");
    const auto unsupported=player.unsupportedEvents();
    const uint8_t unattached[]{0xf0,0x41,0x10,0x42,0x11,0x48,0,0,0,0,2,0x36,0xf7,0xb0,7,87};
    player.push(unattached); player.renderFrames(3200);
    if(player.bulkReplyPending() || player.failed() || player.unsupportedEvents()!=unsupported+1
        || player.partSettings().parts[1].controls.volume!=87)
        throw std::runtime_error("Unconnected bulk output stalled MIDI");
    std::printf("Native bulk lifecycle: %u requests, %u H8 packets match; stop/reset, RX discard/restart, TX completion and40-tick spacing checked\n",requests,packets);
}
