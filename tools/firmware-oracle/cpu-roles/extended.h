#pragma once
#include "mcu_interrupt.h"
#include "sc55_sysex.h"

// Read-only observation of firmware reached through MIDI input. All changes
// are confined to the diagnostic emulator, not the plug-in or its state.
inline void TraceExtendedSystem(Emulator& emu,bool supplyTxReadyInterrupt=false)
{
    auto& cpu=emu.GetMCU();
    auto word=[&](unsigned p){return MCU_Read16(cpu,p);};
    auto run=[&](unsigned duration,const char* label) {
        const auto end=cpu.cycles+duration;
        std::map<unsigned,unsigned> hits;
        std::vector<uint8_t> output;
        while(cpu.cycles<end) {
            // Explicit diagnostic A/B only. Product UART currently omits the
            // ready IRQ when TIE is enabled while TDRE is already set.
            if(supplyTxReadyInterrupt && (cpu.dev_register[DEV_SCR]&0xa0)==0xa0
                && (cpu.dev_register[DEV_SSR]&0x80))
                MCU_Interrupt_SetRequest(cpu,INTERRUPT_SOURCE_UART_TX,1);
            const unsigned pc=(unsigned(cpu.cp)<<16)|cpu.pc;
            switch(pc) {
                case 0x047321: case 0x0415da: case 0x041680: case 0x0417ce:
                case 0x04192c: case 0x041c3f: case 0x041cab: case 0x041e6a:
                case 0x041617: case 0x0408b8: ++hits[pc];
            }
            const auto before=word(0xabfc);
            emu.Step();
            const auto after=word(0xabfc);
            if(before<256 && after<256 && before!=after) {
                // Normal byte append, or checksum + F7 append. A reset or
                // rollback of the output queue is not transmission.
                const unsigned count=(after-before)&255;
                if(count<=2) for(unsigned i=0;i<count;++i)
                    output.push_back(MCU_Read(cpu,0xa5f8+((before+i)&255)));
            }
        }
        for(auto [pc,count]:hits) std::printf("EXT %s pc=%06x count=%u\n",label,pc,count);
        if(supplyTxReadyInterrupt && std::strncmp(label,"request-",8)==0
            && word(0xabfa)!=word(0xabfc))
            throw std::runtime_error("Diagnostic TX ready interrupt did not drain the reply");
        std::printf("END %s pc=%02x:%04x txRead=%u txWrite=%u scr=%02x ssr=%02x\n",
            label,cpu.cp,cpu.pc,word(0xabfa),word(0xabfc),cpu.dev_register[DEV_SCR],cpu.dev_register[DEV_SSR]);
        if(!output.empty()) {
            std::printf("TX_QUEUE %s",label);
            for(auto byte:output) std::printf(" %02x",byte);
            std::printf("\n");
        }
    };
    auto send=[&](unsigned model,unsigned command,std::initializer_list<uint8_t> payload,const char* label) {
        std::vector<uint8_t> msg{0xf0,0x41,0x10,uint8_t(model),uint8_t(command)};
        unsigned sum=0;
        for(auto byte:payload) {msg.push_back(byte);sum+=byte;}
        msg.push_back(uint8_t((128-(sum&127))&127)); msg.push_back(0xf7);
        emu.PostMIDI(msg); run(4000000,label);
    };
    run(120000000,"boot");
    for(unsigned button : {unsigned(MCU_BUTTON_INST_R),unsigned(MCU_BUTTON_LEVEL_R)}) {
        std::array<uint8_t,0x748> before{};
        for(unsigned i=0;i<before.size();++i) before[i]=MCU_Read(cpu,0x8000+i);
        cpu.button_pressed.store(1u<<button);
        run(2000000,"panel-down");
        cpu.button_pressed.store(0);
        run(2000000,"panel-up");
        for(unsigned i=0;i<before.size();++i) {
            const auto value=MCU_Read(cpu,0x8000+i);
            if(before[i]!=value) std::printf("PANEL button=%u ram=%04x old=%02x new=%02x\n",button,0x8000+i,before[i],value);
        }
    }
    send(0x45,0x12,{0x10,0,0,'T','E','S','T'},"display-text");
    sc55::DisplayData nativeDisplay;
    const uint8_t displayText[]{0x10,0,0,'T','E','S','T'};
    if(nativeDisplay.write(displayText)!=sc55::DisplayData::WriteResult::applied)
        throw std::runtime_error("Native display rejected text");
    if(MCU_Read(cpu,0xcead)!=4 || MCU_Read(cpu,0xceb0)!='T' || MCU_Read(cpu,0xceb3)!='T')
        throw std::runtime_error("Display text did not reach firmware state");
    std::printf("DISPLAY length=%u text=",MCU_Read(cpu,0xcead));
    for(unsigned i=0;i<4;++i) std::printf("%02x ",MCU_Read(cpu,0xceb0+i));
    std::printf("\n");
    for(unsigned i=0;i<nativeDisplay.textLength;++i)
        if(nativeDisplay.text[i]!=MCU_Read(cpu,0xceb0+i))
            throw std::runtime_error("Native display text differs from ROM");
    {
        std::vector<uint8_t> msg{0xf0,0x41,0x10,0x45,0x12,0x10,1,0};
        unsigned sum=0x11;
        for(unsigned i=0;i<64;++i) {msg.push_back(uint8_t(i));sum+=i;}
        msg.push_back(uint8_t((128-(sum&127))&127));msg.push_back(0xf7);
        emu.PostMIDI(msg);run(4000000,"display-dots");
        if(nativeDisplay.write(std::span(msg).subspan(5,67))!=sc55::DisplayData::WriteResult::applied)
            throw std::runtime_error("Native display rejected bitmap");
        unsigned mismatches=0;
        for(unsigned i=0;i<64;++i) mismatches+=MCU_Read(cpu,0xff00+i)!=nativeDisplay.bitmap[i];
        std::printf("DISPLAY dotsMismatches=%u timer=%u\n",mismatches,word(0xcf34));
        if(mismatches) throw std::runtime_error("Display dot payload mismatch");
    }
    send(0x42,0x12,{0x48,0,4,2,5},"bulk-master");
    std::printf("BULK master=%02x\n",MCU_Read(cpu,0x8002));
    if(MCU_Read(cpu,0x8002)!=0x25) throw std::runtime_error("Bulk master value mismatch");
    send(0x42,0x12,{0x49,2,0,3,9},"bulk-drum");
    std::printf("BULK drum8848=%02x\n",MCU_Read(cpu,0x8848));
    if(MCU_Read(cpu,0x8848)!=0x39) throw std::runtime_error("Bulk drum value mismatch");
    send(0x42,0x11,{0x40,0,4,0,0,1},"request-master");
    if(!supplyTxReadyInterrupt) {emu.Reset(); cpu.native_v121_enabled=false; run(120000000,"reboot-from-tx-wait");}
    send(0x42,0x11,{0x41,2,0,0,0,1},"request-drum");
    if(!supplyTxReadyInterrupt) {emu.Reset(); cpu.native_v121_enabled=false; run(120000000,"reboot-from-tx-wait");}
    send(0x42,0x11,{0x48,0,4,0,0,2},"request-bulk-master");
    if(!supplyTxReadyInterrupt) {emu.Reset(); cpu.native_v121_enabled=false; run(120000000,"reboot-from-tx-wait");}
    send(0x42,0x11,{0x49,2,0,0,2,0},"request-bulk-drum");
}
