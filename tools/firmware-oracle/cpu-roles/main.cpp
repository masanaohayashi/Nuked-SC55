#include "emu.h"
#include "rom_loader.h"
#include <map>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <vector>
#include "extended.h"
#include "effects.h"
#include "effects-requests.h"
#include "effects-settings.h"
#include "rpn.h"
#include "nrpn.h"
#include "mono.h"
#include "master.h"
#include "parts.h"
#include "all-notes.h"
#include "rhythm-presets.h"
#include "melodic-presets.h"
#include "system-defaults.h"
#include "native-synth.h"
#include "mcu_timer.h"
#include "voice-renderer-clock.h"
#include "chorus-setup.h"
#include "reverb-setup.h"
#include "effect-transitions.h"
#include "voice-stop.h"
#include "NukedSC55Emulator.h"
#include "SC55Lcd.h"
#include <fstream>
#include "song-allocation.h"
#include "bulk-system.h"
#include "parameter-reply.h"
#include "bulk-reply.h"
#include "control-work.h"
#include "control-groups.h"
#include "display-control.h"
#include "midi-input-settings.h"
#include "source-controller.h"
#include "parameter-transport.h"
#include "panel-settings.h"
#include "panel-bulk.h"
#include "release-integration.h"
#include "startup-wake.h"
#include "midi-during-prepare.h"
#include "kernel-events.h"
#include "configuration-readers.h"

namespace {
ControlWorkProbe* activeControlWork=nullptr;
KernelEventProbe* activeKernelEvents=nullptr;
ConfigurationReaders* activeConfigurationReaders=nullptr;
std::function<void(const mcu_t&)> controlScheduleInstruction;
bool trackControlInterrupts=false;
struct HardwareFrame { const mcu_t* cpu; uint16_t stack,task; };
std::vector<HardwareFrame> hardwareFrames;
}
void Oracle_H8InterruptEntered(const mcu_t& source,uint32_t,int32_t mask)
{
    if(!trackControlInterrupts || mask<0) return; // TRAPA/exception frames are not hardware IRQ work.
    auto& cpu=const_cast<mcu_t&>(source); // MCU_Read is not const-qualified.
    const auto task=MCU_Read16(cpu,0xfdca);
    // Track every context for whole-pass partitioning, including interrupts
    // that arrive while display or voice-admission work preempts task8.
    for(const auto& frame:hardwareFrames)
        if(frame.cpu==&source && frame.stack==source.r[7])
            throw std::runtime_error("IRQ stack frame was reused without an observed return");
    hardwareFrames.push_back({&source,source.r[7],task});
}
void Oracle_H8InterruptReturn(const mcu_t& source,unsigned restoredStatusBytes)
{
    // The kernel can suspend an IRQ frame and restore another task's stack.
    // Hardware frames therefore do not form one global chronological stack.
    std::erase_if(hardwareFrames,[&](const auto& frame) {
        return frame.cpu==&source && uint16_t(frame.stack+restoredStatusBytes)==source.r[7];
    });
}
void Oracle_ConfigurationRead(const mcu_t& source,uint32_t address)
{
    if(activeConfigurationReaders) activeConfigurationReaders->read(source,address);
}
void Oracle_H8Fallback(const mcu_t& source)
{
    if(activeConfigurationReaders) activeConfigurationReaders->instruction(source);
    // 04af installs the idle scheduler stack afresh. Task9 is not a saved
    // task-table entry, so its old hardware frame is abandoned, not RTE'd.
    if(trackControlInterrupts && source.cp==0 && source.pc==0x4af)
        std::erase_if(hardwareFrames,[&](const auto& frame) {return frame.cpu==&source && frame.task==9;});
    if(activeKernelEvents) activeKernelEvents->instruction(const_cast<mcu_t&>(source));
    if(controlScheduleInstruction) controlScheduleInstruction(source);
    if(panelBulkInstruction) panelBulkInstruction((unsigned(source.cp)<<16)|source.pc);
    if(!activeControlWork) return;
    auto& cpu=const_cast<mcu_t&>(source);
    const auto task=MCU_Read16(cpu,0xfdca);
    const bool inIrq=std::any_of(hardwareFrames.begin(),hardwareFrames.end(),[&](const auto& frame) {
        return frame.cpu==&source && frame.task==task;
    });
    activeControlWork->instruction(cpu,inIrq);
}
int main(int argc,char** argv) {
    if(argc < 2 || argc > 4) return 2;
    trackControlInterrupts=std::getenv("SC55_TRACE_CONTROL_ROUTINES")!=nullptr
        || (argc>=3 && std::strcmp(argv[2],"--native-controller-work")==0);
    common::LoadRomsetResult roms;
    if(common::LoadRomset(argv[1],{},common::RomLoader::Hashing,{},roms) != common::LoadRomsetError{}) return 3;
    if(roms.picked_name != "mk1-v1.21") return 4;
    Emulator emu;
    if(!emu.Init({}) || !emu.LoadRoms(roms.romset,roms.romset_info)) return 5;
    emu.Reset(); auto& cpu=emu.GetMCU(); cpu.native_v121_enabled=false;
    if(argc==3 && std::strcmp(argv[2],"--native-midi-during-preparation")==0) {
        VerifyMidiDuringPreparation(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-program-during-preparation")==0) {
        VerifyMidiDuringPreparation(emu,roms.romset_info,true); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-mode-during-preparation")==0) {
        VerifyMidiDuringPreparation(emu,roms.romset_info,false,true); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-sysex-during-preparation")==0) {
        VerifyMidiDuringPreparation(emu,roms.romset_info,false,false,true); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-part-during-preparation")==0) {
        VerifyMidiDuringPreparation(emu,roms.romset_info,false,false,false,true); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-startup-wake")==0) {
        VerifyStartupWake(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-boundary-reception")==0) {
        VerifyStartupWake(emu,roms.romset_info,true); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-command-control-order")==0) {
        VerifyCommandControlOrder(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-panel-during-startup")==0) {
        VerifyPanelSettings(emu,roms.romset_info,true); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-high-admission")==0) {
        VerifyReleaseIntegration(emu,roms.romset_info,true); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-shared-rhythm")==0) {
        VerifyReleaseIntegration(emu,roms.romset_info,false,true); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-release-integration")==0) {
        VerifyReleaseIntegration(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-panel-solo")==0) {
        VerifyPanelSettings(emu,roms.romset_info,false,true); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--panel-options")==0) {
        InspectPanelOptions(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--midi-input-panel-route")==0) {
        InspectMidiInputPanel(emu); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--kernel-notify-sites")==0) {
        const auto& raw=roms.romset_info.rom_data;
        ReportKernelNotificationSites(raw[size_t(RomLocation::ROM1)],raw[size_t(RomLocation::ROM2)]);
        return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-panel-settings")==0) {
        VerifyPanelSettings(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-parameter-transport")==0) {
        VerifyParameterTransport(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-source-controller")==0) {
        VerifySourceController(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-display-control")==0) {
        VerifyDisplayControl(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-midi-input-settings")==0) {
        VerifyMidiInputSettings(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--panel-bulk-sequence")==0) {
        VerifyPanelBulkSequence(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--panel-bulk-system-parts")==0) {
        VerifyPanelBulkSequence(emu,roms.romset_info,sc55::BulkReplyTransfer::PanelScope::systemAndParts); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--panel-bulk-parts")==0) {
        VerifyPanelBulkSequence(emu,roms.romset_info,sc55::BulkReplyTransfer::PanelScope::parts); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--panel-bulk-melodic")==0) {
        VerifyPanelBulkSequence(emu,roms.romset_info,sc55::BulkReplyTransfer::PanelScope::parts,PanelBulkSelection::melodicOnly); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--panel-bulk-empty")==0) {
        VerifyPanelBulkSequence(emu,roms.romset_info,sc55::BulkReplyTransfer::PanelScope::parts,PanelBulkSelection::none); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--panel-bulk-both-maps")==0) {
        VerifyPanelBulkSequence(emu,roms.romset_info,sc55::BulkReplyTransfer::PanelScope::systemAndParts,PanelBulkSelection::bothMaps); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--panel-bulk-second-map")==0) {
        VerifyPanelBulkSequence(emu,roms.romset_info,sc55::BulkReplyTransfer::PanelScope::parts,PanelBulkSelection::secondMapOnly); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-bulk-replies")==0) {
        VerifyBulkReplies(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-parameter-replies")==0) {
        VerifyParameterReplies(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-reset-reply-timing")==0) {
        VerifyParameterReplies(emu,roms.romset_info,true); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-reset-reply-minimal")==0) {
        VerifyParameterReplies(emu,roms.romset_info,true,true); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--native-bulk-system")==0) {
        VerifyBulkSystem(emu,roms.romset_info); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--midi-watchdog")==0) {
        // Observe the real UART watchdog, not a MIDI-spec timeout assumption.
        const auto boot=cpu.cycles+120000000;
        while(cpu.cycles<boot) emu.Step();
        const auto& raw=roms.romset_info.rom_data;
        const auto& rom1=raw[size_t(RomLocation::ROM1)];
        const auto& rom2=raw[size_t(RomLocation::ROM2)];
        sc55::NativeSynth native(sc55::ImportSoundData(rom1,rom2),rom1,rom2,
            raw[size_t(RomLocation::WAVEROM1)],raw[size_t(RomLocation::WAVEROM2)],raw[size_t(RomLocation::WAVEROM3)]);
        std::array<AudioFrame<int32_t>,257> frames{};
        for(unsigned i=0;i<64;++i) native.render(frames);
        std::printf("WATCH_TIMER tcr=%02x tcsr=%02x ocra=%02x%02x\n",
            MCU_Read(cpu,0xff90),MCU_Read(cpu,0xff91),MCU_Read(cpu,0xff94),MCU_Read(cpu,0xff95));
        unsigned checks=0,timeouts=0;
        unsigned received=0,verifiedPolls=0;
        sc55::MidiReceiveState receiver;
        sc55::MidiReceiveTimer receiverTimer(MCU_Read(cpu,0xac29));
        if(MCU_Read(cpu,0xac27)!=receiver.flags()) throw std::runtime_error("Unexpected boot receiver flags");
        std::optional<bool> forwarded;
        bool pollPending=false;
        uint64_t previous=0;
        auto run=[&](uint64_t duration) {
            const auto end=cpu.cycles+duration;
            while(cpu.cycles<end) {
                if(cpu.cp==0 && cpu.pc==0x0698) {
                    const auto word=[&](unsigned address) {
                        return unsigned(MCU_Read(cpu,address))*256+MCU_Read(cpu,address+1);
                    };
                    (void)receiverTimer.tick(receiver,word(0xabfa)==word(0xabfc),
                        (cpu.dev_register[DEV_SSR]&0x80)!=0);
                    pollPending=true;
                }
                if(cpu.cp==0 && cpu.pc==0x05b3) {
                    forwarded=receiver.receiveByte(uint8_t(cpu.r[6]));
                    ++received;
                }
                if(forwarded && cpu.cp==0 && (cpu.pc==0x05e8 || cpu.pc==0x0662)) {
                    if(*forwarded!=(cpu.pc==0x05e8) || receiver.flags()!=MCU_Read(cpu,0xac27))
                        throw std::runtime_error("Native UART ingress differs from H8");
                    forwarded.reset();
                }
                if(cpu.cp==0 && cpu.pc==0x06e0) {
                    std::printf("WATCH cycle=%llu interval=%llu flags=%02x divider=%02x\n",
                        (unsigned long long)cpu.cycles,(unsigned long long)(previous?cpu.cycles-previous:0),
                        MCU_Read(cpu,0xac27),MCU_Read(cpu,0xac29));
                    previous=cpu.cycles; ++checks;
                }
                if(pollPending && cpu.cp==0 && (cpu.pc==0x06d9 || cpu.pc==0x0720)) {
                    if(receiver.flags()!=MCU_Read(cpu,0xac27) || receiverTimer.divider()!=MCU_Read(cpu,0xac29))
                        throw std::runtime_error("Native watchdog transition differs from H8");
                    pollPending=false; ++verifiedPolls;
                }
                if(cpu.cp==0 && cpu.pc==0x06ec) {
                    ++timeouts;
                    std::printf("TIMEOUT cycle=%llu voices=%06x\n",
                        (unsigned long long)cpu.cycles,emu.GetPCM().voice_mask);
                }
                if(cpu.cp==4 && cpu.pc==0x08b8)
                    std::printf("WATCH_RECOVERY cycle=%llu\n",(unsigned long long)cpu.cycles);
                emu.Step();
            }
            auto remaining=duration/625;
            while(remaining) {
                const auto count=std::min<uint64_t>(remaining,frames.size());
                native.render(std::span(frames).first(count)); remaining-=count;
            }
            if(native.failed()) throw std::runtime_error("Native watchdog rendering failed");
        };
        const uint8_t notes[]{0xc0,48,0xb0,7,93,11,32,0x90,60,100,
            0xc1,80,0xb1,64,127,11,25,0x91,64,100,0x81,64,0};
        emu.PostMIDI(notes); native.push(notes); run(16000000);
        if(timeouts) throw std::runtime_error("Watchdog armed without FE");
        if(native.state().parts[1].expression!=32 || native.state().parts[2].expression!=25)
            throw std::runtime_error("Unarmed receiver reset controllers");
        const uint8_t sensing[]{0xfe};
        emu.PostMIDI(sensing); native.push(sensing); run(24000000);
        if(timeouts!=1) throw std::runtime_error("Missing single active-sensing timeout");
        const auto recovered=native.state();
        for(unsigned part=0;part<16;++part) {
            if(recovered.parts[part].expression!=MCU_Read(cpu,0xab36+part)
                || recovered.parts[part].voices!=MCU_Read(cpu,0xa1f0+part)) {
                std::printf("RECOVERY part=%u expression native/H8=%u/%u voices=%u/%u\n",part,
                    recovered.parts[part].expression,MCU_Read(cpu,0xab36+part),
                    recovered.parts[part].voices,MCU_Read(cpu,0xa1f0+part));
                throw std::runtime_error("Native communication recovery differs from H8");
            }
        }
        if(recovered.parts[1].expression!=127 || recovered.parts[1].volume!=93
            || recovered.parts[1].program!=48 || recovered.parts[1].voices || recovered.parts[2].voices)
            throw std::runtime_error("Recovery did not preserve settings or stop held/pedal-held notes");
        std::puts("Native recovery: all 16 expressions/voice counts match; program/volume preserved, held and pedal-held keys stopped");
        const uint8_t mixed[]{0x90,60,0,0xf1,1,61,100,0xf8,0xfa,0xfb,0xfc,0xff,
            0xf7,62,100,0x90,63,100,0xf2,1,2,0xf3,1,0xf4,0xf5,0xf6,0xf0,0x41,0xf8,0xf7};
        emu.PostMIDI(mixed); native.push(mixed); run(2000000);
        const auto resumed=native.state();
        for(unsigned part=0;part<16;++part)
            if(resumed.parts[part].voices!=MCU_Read(cpu,0xa1f0+part))
                throw std::runtime_error("Post-recovery note allocation differs from H8");
        if(!resumed.parts[1].voices) throw std::runtime_error("Receiver cannot restart notes after recovery");
        if(forwarded || pollPending || received!=sizeof(notes)+sizeof(sensing)+sizeof(mixed)
            || verifiedPolls<checks) throw std::runtime_error("Incomplete receive-state coverage");
        std::printf("Native receive state: %u bytes and %u watchdog polls match H8\n",received,verifiedPolls);
        std::printf("WATCHDOG checks=%u timeouts=%u voices=%06x\n",checks,timeouts,emu.GetPCM().voice_mask);
        return 0;
    }
    if(argc==4 && std::strcmp(argv[2],"--song-allocation")==0)
        return CompareSongAllocation(emu,roms.romset_info,argv[3]);
    if(argc==4 && std::strcmp(argv[2],"--song-part16")==0)
        return CompareSongAllocation(emu,roms.romset_info,argv[3],true);
    if(argc==4 && std::strcmp(argv[2],"--song-first-kick")==0)
        return CompareSongAllocation(emu,roms.romset_info,argv[3],false,true);
    if(argc==3 && std::strcmp(argv[2],"--direct-voice-stop")==0) { VerifyDirectVoiceStop(); return 0; }
    if(argc==3 && std::strcmp(argv[2],"--direct-effect-transitions")==0) {
        const auto& raw=roms.romset_info.rom_data;
        VerifyDirectEffectTransitions(sc55::ImportEffectsTables(raw[size_t(RomLocation::ROM1)],raw[size_t(RomLocation::ROM2)]));
        return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--direct-reverb-setup")==0) {
        const auto& raw=roms.romset_info.rom_data;
        VerifyDirectReverbSetup(sc55::ImportEffectsTables(raw[size_t(RomLocation::ROM1)],raw[size_t(RomLocation::ROM2)]));
        return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--direct-chorus-setup")==0) {
        VerifyDirectChorusSetup(); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--independent-synth")==0) {
        VerifyNativeSynth(emu,roms.romset_info,false,false,1,true); return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--independent-signal")==0) {
        for(uint8_t program: {uint8_t(48),uint8_t(80),uint8_t(120)})
            VerifyVoiceRenderingClock(roms.romset_info,roms.romset,program,true,true);
        return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--direct-voice-lifecycle")==0) {
        auto direct=std::make_unique<pcm_t>(),reference=std::make_unique<pcm_t>();
        uint32_t random=7163;
        auto next=[&] { random=random*1664525u+1013904223u; return random; };
        const auto read=[&](uint8_t a) { return PCM_Read(*reference,a); };
        const auto write=[&](uint8_t a,uint8_t v) { PCM_Write(*reference,a,v); };
        for(unsigned n=0;n<4096;++n) {
            const auto slot=uint8_t(n%24);
            const auto keys=next();
            direct->voice_mask_pending=reference->voice_mask_pending=next();
            PCM_CommitVoiceKeys(*direct,keys);
            for(unsigned i=0;i<4;++i) write(uint8_t(i),uint8_t(keys>>(24-8*i)));
            (void)read(0);
            for(unsigned i=0;i<3;++i)
                direct->ram2[slot][9+i]=reference->ram2[slot][9+i]=uint16_t(next());
            direct->ram2[slot][7]=reference->ram2[slot][7]=uint16_t(next());
            direct->write_latch=reference->write_latch=next()&0xfffff;
            const auto levels=PCM_VoiceGainLevels(*direct,slot);
            write(0x3e,slot);
            for(unsigned i=0;i<2;++i) {
                (void)read(uint8_t(0x32+2*i));
                const auto high=read(0x3a),low=read(0x3b);
                if(levels[i]!=uint16_t((high<<8)|low)) throw std::runtime_error("Voice gain readback differs");
            }
            sc55::VoicePostEnable post{};
            post.level=uint16_t(next()); post.command=uint16_t(next());
            const auto actual=PCM_CompleteVoiceEnable(*direct,slot,post.level,post.command);
            const auto expected=sc55::PollVoicePostEnable(slot,post,read,write);
            if(!expected || actual!=*expected
                || direct->voice_mask!=reference->voice_mask
                || direct->voice_mask_pending!=reference->voice_mask_pending
                || direct->voice_mask_updating!=reference->voice_mask_updating
                || std::memcmp(direct->ram2,reference->ram2,sizeof(direct->ram2))
                || direct->write_latch!=reference->write_latch || direct->read_latch!=reference->read_latch
                || direct->select_channel!=reference->select_channel || direct->sim_dirty!=reference->sim_dirty)
                throw std::runtime_error("Direct voice lifecycle differs from register transaction");
        }
        std::puts("Direct voice lifecycle: 4096 key commits, gain snapshots and pending/ready enable transactions match");
        return 0;
    }
    if(argc==3 && (std::strcmp(argv[2],"--direct-voice-start")==0 || std::strcmp(argv[2],"--direct-renderer-start")==0)) {
        const bool simulated=std::strcmp(argv[2],"--direct-renderer-start")==0;
        auto direct=std::make_unique<pcm_t>(),reference=std::make_unique<pcm_t>();
        if(simulated) for(auto* pcm:{direct.get(),reference.get()}) {
            pcm->is_mk1=true; pcm->config.reg_slots=24; PCM_UseSimulation(*pcm,true);
            for(unsigned slot=0;slot<24;++slot) {
                pcm->sim.address[slot]=int32_t(1234+slot);
                pcm->sim.svf_low[slot]=float(slot+1); pcm->sim.svf_band[slot]=float(slot+2);
            }
        }
        struct Writer {
            pcm_t& pcm;
            void operator()(uint8_t a,uint8_t v) const { PCM_Write(pcm,a,v); }
            void installVoice(uint8_t slot,const sc55::VoiceRenderStart& start) const
            { PCM_InstallVoice(pcm,slot,start); }
        };
        uint32_t random=7163;
        auto next=[&] { random=random*1664525u+1013904223u; return uint16_t(random>>16); };
        for(unsigned n=0;n<4096;++n) {
            sc55::VoiceStopState state;
            state.flagMinus3B=uint8_t(n&128); state.delayAccumulator=n&1 ? next() : 0;
            state.cached16=next(); state.cached18=next(); state.pcm10=next();
            state.savedStage=uint16_t((n%7)*2); state.progress=next();
            auto expected=state;
            sc55::PreparedVoicePcm prepared{{next(),uint32_t(next())<<8|next(),
                uint32_t(next())<<8|next(),uint32_t(next())<<8|next()},next(),next(),next(),next()};
            const auto slot=uint8_t(n%24);
            if(simulated) for(unsigned i=0;i<3;++i)
                direct->ram2[slot][9+i]=reference->ram2[slot][9+i]=next();
            sc55::CommitPreparedVoice(slot,state,prepared,Writer{*direct});
            sc55::CommitPreparedVoice(slot,expected,prepared,
                [&](uint8_t a,uint8_t v) { PCM_Write(*reference,a,v); });
            if(std::memcmp(direct->ram1,reference->ram1,sizeof(direct->ram1))
                || std::memcmp(direct->ram2,reference->ram2,sizeof(direct->ram2))
                || direct->write_latch!=reference->write_latch || direct->read_latch!=reference->read_latch
                || direct->select_channel!=reference->select_channel || (!simulated && direct->sim_dirty!=reference->sim_dirty)
                || state.stages!=expected.stages || state.cached18!=expected.cached18
                || state.progress!=expected.progress || state.savedStage!=expected.savedStage)
                throw std::runtime_error("Direct voice installation differs from register commit");
            if(simulated) {
                PCMSim_SyncVoice(reference->sim,*reference,slot);
                const auto& a=direct->sim; const auto& b=reference->sim;
                if((direct->sim_dirty&(1u<<slot)) || a.address_loop[slot]!=b.address_loop[slot]
                    || a.address_end[slot]!=b.address_end[slot] || a.bank[slot]!=b.bank[slot]
                    || a.rom_mask[slot]!=b.rom_mask[slot] || a.phase_step[slot]!=b.phase_step[slot]
                    || a.bidi_mask[slot]!=b.bidi_mask[slot] || a.direction[slot]!=b.direction[slot]
                    || a.pan_l[slot]!=b.pan_l[slot] || a.pan_r[slot]!=b.pan_r[slot]
                    || a.send_reverb[slot]!=b.send_reverb[slot] || a.send_chorus[slot]!=b.send_chorus[slot]
                    || a.svf_q[slot]!=b.svf_q[slot] || a.svf_tap[slot]!=b.svf_tap[slot]
                    || a.address[slot]!=b.address[slot] || a.svf_low[slot]!=b.svf_low[slot]
                    || a.svf_band[slot]!=b.svf_band[slot])
                    throw std::runtime_error("Prepared waveform/controls differ or installation reset live history");
                for(unsigned i=0;i<3;++i)
                    if(a.envelopes[slot].ramps[i].command!=b.envelopes[slot].ramps[i].command
                        || a.envelopes[slot].ramps[i].level!=b.envelopes[slot].ramps[i].level)
                        throw std::runtime_error("Prepared voice changed live envelope state");
            }
        }
        std::puts("Direct voice start: 4096 transactions match sample/control registers, latches and activation state");
        if(simulated) std::puts("Prepared renderer start: waveform/controls and retained oscillator/filter/envelope histories match without deferred slot import");
        return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--direct-envelope-readback")==0) {
        auto direct=std::make_unique<pcm_t>(),reference=std::make_unique<pcm_t>();
        uint32_t random=7163;
        auto next=[&] { random=random*1664525u+1013904223u; return uint16_t(random>>16); };
        const auto read=[&](uint8_t address) { return PCM_Read(*reference,address); };
        const auto write=[&](uint8_t address,uint8_t value) { PCM_Write(*reference,address,value); };
        for(unsigned n=0;n<4096;++n) {
            const auto slot=uint8_t(n%24);
            std::array<uint16_t,3> commands{},levels{},expected{};
            direct->write_latch=reference->write_latch=0xa0000u|next();
            direct->read_latch=reference->read_latch=0xb0000u|next();
            direct->sim_dirty=reference->sim_dirty=0;
            for(unsigned i=0;i<3;++i) {
                commands[i]=(n&(1u<<i)) ? uint16_t(0xff00) : next();
                levels[i]=next();
                direct->ram2[slot][9+i]=reference->ram2[slot][9+i]=next();
            }
            const auto actual=PCM_SynchronizeVoiceEnvelopes(*direct,slot,commands,levels);
            write(0x3e,slot);
            for(unsigned i=0;i<3;++i)
                expected[i]=sc55::SynchronizePcmRamp(commands[i],levels[i],uint8_t(0x16+2*i),
                    uint8_t(0x32+2*i),read,write);
            if(actual!=expected || std::memcmp(direct->ram2,reference->ram2,sizeof(direct->ram2))
                || direct->write_latch!=reference->write_latch || direct->read_latch!=reference->read_latch
                || direct->select_channel!=reference->select_channel || direct->sim_dirty!=reference->sim_dirty)
                throw std::runtime_error("Direct envelope readback differs from register synchronization");
        }
        std::puts("Direct envelope readback: 4096 mixed held/running transactions match levels, registers and latches");
        return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--direct-voice-updates")==0) {
        auto direct=std::make_unique<pcm_t>(),reference=std::make_unique<pcm_t>();
        direct->use_simulation=reference->use_simulation=true;
        direct->config.reg_slots=reference->config.reg_slots=24;
        direct->write_latch=reference->write_latch=0xa0000;
        for(unsigned slot=0;slot<24;++slot) {
            direct->ram2[slot][7]=reference->ram2[slot][7]=uint16_t((slot+1)%24);
            direct->sim.svf_low[slot]=reference->sim.svf_low[slot]=float(slot+1);
            for(unsigned envelope=0;envelope<3;++envelope) {
                const auto level=uint16_t((slot+1)*137+envelope);
                direct->ram2[slot][9+envelope]=reference->ram2[slot][9+envelope]=level;
                direct->sim.envelopes[slot].ramps[envelope].level=level;
            }
        }
        uint32_t random=7163;
        auto next=[&] { random=random*1664525u+1013904223u; return uint16_t(random>>16); };
        for(unsigned i=0;i<4096;++i) {
            const unsigned slot=i%24;
            const sc55::VoiceRenderUpdate update{next(),{next(),next(),next()},
                int8_t(next()),int8_t(next()),int8_t(next()),int8_t(next()),uint8_t(next()),uint8_t(next())};
            PCM_ApplyVoiceUpdate(*direct,slot,update);
            const auto word=[&](unsigned address,uint16_t value) {
                PCM_Write(*reference,address,uint8_t(value>>8)); PCM_Write(*reference,address+1,uint8_t(value));
            };
            PCM_Write(*reference,0x3e,uint8_t(slot));
            word(0x18,update.rampCommands[1]); word(0x16,update.rampCommands[0]);
            word(0x12,uint16_t((uint16_t(uint8_t(update.panLeft))<<8)|uint8_t(update.panRight)));
            word(0x14,uint16_t((uint16_t(uint8_t(update.reverbSend))<<8)|uint8_t(update.chorusSend)));
            word(0x1a,update.rampCommands[2]);
            word(0x1c,uint16_t((uint16_t(update.resonance)<<8)|update.filterFlags));
            word(0x10,update.phaseIncrement);
            if(std::memcmp(direct->ram2,reference->ram2,sizeof(direct->ram2))
                || direct->select_channel!=reference->select_channel || direct->write_latch!=reference->write_latch
                || direct->read_latch!=reference->read_latch)
                throw std::runtime_error("Direct voice update differs from byte-register transaction");
            const uint16_t loopPitch=uint16_t(~update.phaseIncrement);
            direct->sim_dirty=0;
            PCM_SetVoicePitch(*direct,slot,loopPitch);
            word(0x10,loopPitch);
            if(direct->sim_dirty || std::memcmp(direct->ram2,reference->ram2,sizeof(direct->ram2))
                || direct->write_latch!=reference->write_latch)
                throw std::runtime_error("Loop-pitch update changed state or invalidated waveform setup");
            for(unsigned voice=0;voice<24;++voice) {
                PCMSim_SyncVoice(reference->sim,*reference,int(voice));
                if(direct->sim.phase_step[voice]!=reference->sim.phase_step[voice]
                    || direct->sim.svf_low[voice]!=reference->sim.svf_low[voice]
                    || direct->sim.pan_l[voice]!=reference->sim.pan_l[voice]
                    || direct->sim.pan_r[voice]!=reference->sim.pan_r[voice]
                    || direct->sim.send_reverb[voice]!=reference->sim.send_reverb[voice]
                    || direct->sim.send_chorus[voice]!=reference->sim.send_chorus[voice]
                    || direct->sim.svf_q[voice]!=reference->sim.svf_q[voice]
                    || direct->sim.svf_tap[voice]!=reference->sim.svf_tap[voice])
                    throw std::runtime_error("Direct renderer update changed history or lost linked pitch");
                for(unsigned envelope=0;envelope<3;++envelope)
                    if(direct->sim.envelopes[voice].ramps[envelope].command!=reference->sim.envelopes[voice].ramps[envelope].command
                        || direct->sim.envelopes[voice].ramps[envelope].level!=reference->sim.envelopes[voice].ramps[envelope].level)
                        throw std::runtime_error("Direct renderer update changed envelope history or command");
            }
        }
        std::puts("Direct voice updates: 4096 transactions match register/latch state and linked renderer pitch");
        return 0;
    }
    if(argc==3 && std::strcmp(argv[2],"--float-effects-audio")==0) {
        if(!emu.GetPCM().use_float_effects) throw std::runtime_error("Set SC55_FXSIM=1");
        const auto run=[&](uint64_t count) { const auto end=cpu.cycles+count; while(cpu.cycles<end) emu.Step(); };
        run(120000000);
        struct Capture { uint64_t hash=1469598103934665603ull,frames=0; } capture;
        emu.SetSampleCallback([](void* context,const AudioFrame<int32_t>& frame) {
            auto& out=*static_cast<Capture*>(context);
            for(const auto sample:{frame.left,frame.right}) {
                out.hash^=uint32_t(sample); out.hash*=1099511628211ull;
            }
            ++out.frames;
        },&capture);
        const uint8_t notes[]{0xc0,48,0x90,60,100,0x90,67,90};
        emu.PostMIDI(notes); run(20000000);
        for(uint8_t preset=0;preset<8;++preset) {
            for(uint8_t address:{uint8_t(0x30),uint8_t(0x38)}) {
                const uint8_t packet[]{0xf0,0x41,0x10,0x42,0x12,0x40,1,address,preset,
                    uint8_t((128-((0x41+address+preset)&127))&127),0xf7};
                emu.PostMIDI(packet); run(4000000);
            }
        }
        const uint8_t off[]{0xb0,123,0}; emu.PostMIDI(off); run(80000000);
        std::printf("Float effects integration: frames=%llu audio=%016llx phase=%04x spread=%04x taps=%04x,%04x\n",
            (unsigned long long)capture.frames,(unsigned long long)capture.hash,
            emu.GetPCM().ram2[31][8],emu.GetPCM().ram2[30][9],
            emu.GetPCM().ram2[29][10],emu.GetPCM().ram2[29][11]);
        return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--panel-semantics") == 0) {
        const auto& raw=roms.romset_info.rom_data;
        const auto encoded=sc55::ImportSoundData(raw[size_t(RomLocation::ROM1)],raw[size_t(RomLocation::ROM2)]);
        sc55::NativeSynth synth(encoded,raw[size_t(RomLocation::ROM1)],raw[size_t(RomLocation::ROM2)],
            raw[size_t(RomLocation::WAVEROM1)],raw[size_t(RomLocation::WAVEROM2)],raw[size_t(RomLocation::WAVEROM3)],
            sc55::NativeSynth::VoiceRendering::nativeVoices);
        auto run=[&](uint64_t cycles) {
            const auto end=cpu.cycles+cycles; while(cpu.cycles<end) emu.Step();
            std::array<AudioFrame<int32_t>,256> output{};
            for(auto remaining=cycles/625;remaining;) {
                const auto count=std::min<uint64_t>(remaining,output.size());
                synth.render(std::span(output).first(count)); remaining-=count;
            }
        };
        run(120000000);
        std::array<uint8_t,0x748> previous{};
        auto capture=[&](const char* label,bool report) {
            std::printf("PANEL %s mask=%06x leds=%02x cdcc=%02x cdf4=%02x cdf5=%02x levels=%u,%u,%u,%u\n",label,
                emu.GetPCM().voice_mask&emu.GetPCM().voice_mask_pending,cpu.io_sd,
                MCU_Read(cpu,0xcdcc),MCU_Read(cpu,0xcdf4),MCU_Read(cpu,0xcdf5),
                emu.GetPCM().ram2[20][10],emu.GetPCM().ram2[21][10],emu.GetPCM().ram2[22][10],emu.GetPCM().ram2[23][10]);
            std::printf("VOICES %s part1=%u part2=%u gainA=%u,%u,%u,%u\n",label,
                MCU_Read(cpu,0xa1f1),MCU_Read(cpu,0xa1f2),emu.GetPCM().ram2[20][9],emu.GetPCM().ram2[21][9],
                emu.GetPCM().ram2[22][9],emu.GetPCM().ram2[23][9]);
            for(unsigned i=0;i<previous.size();++i) {
                const auto now=MCU_Read(cpu,0x8000+i);
                if(report && now!=previous[i])
                    std::printf("SETTING %s address=%04x old=%02x new=%02x\n",label,0x8000+i,previous[i],now);
                previous[i]=now;
            }
            const auto state=synth.state();
            if(state.failed || state.masterVolume!=MCU_Read(cpu,0x8002)
                || state.masterPan!=MCU_Read(cpu,0x8006)
                || state.masterKeyShift!=MCU_Read(cpu,0x8005)
                || state.reverbLevel!=MCU_Read(cpu,0x802d)
                || state.chorusLevel!=MCU_Read(cpu,0x8034)
                || state.globalMuted!=bool(MCU_Read(cpu,0xcdf5)&1)
                || state.allSelected!=bool(MCU_Read(cpu,0xcdcc)&2))
                throw std::runtime_error("Native panel master/mode differs from H8");
            for(unsigned part=0;part<16;++part)
                if(state.parts[part].muted!=!(MCU_Read16(cpu,0x804a+0x70*part)&0x0200)
                    || state.parts[part].voices!=MCU_Read(cpu,0xa1f0+part))
                    throw std::runtime_error("Native panel mute/voice ownership differs from H8");
        };
        capture("boot",false);
        const uint8_t notes[]{0xc0,80,0xc1,80,0x90,60,100,0x91,67,100};
        emu.PostMIDI(notes); synth.push(notes); run(4000000); capture("notes",true);
        auto press=[&](unsigned button,const char* label) {
            if(button==MCU_BUTTON_INST_MUTE) synth.toggleMute();
            else if(button==MCU_BUTTON_INST_ALL) synth.toggleAll();
            else if(button==MCU_BUTTON_LEVEL_L) synth.adjustSelectedPart(sc55::PartParameter::volume,-1);
            else if(button==MCU_BUTTON_PAN_R) synth.adjustSelectedPart(sc55::PartParameter::pan,1);
            else if(button==MCU_BUTTON_KEY_SHIFT_R) synth.adjustSelectedPart(sc55::PartParameter::keyShift,1);
            else if(button==MCU_BUTTON_REVERB_R) synth.adjustSelectedPart(sc55::PartParameter::reverb,1);
            else if(button==MCU_BUTTON_CHORUS_R) synth.adjustSelectedPart(sc55::PartParameter::chorus,1);
            cpu.button_pressed.store(1u<<button); run(1200000);
            cpu.button_pressed.store(0); run(1200000); capture(label,true);
        };
        press(MCU_BUTTON_INST_MUTE,"mute-on");
        emu.PostMIDI(notes); synth.push(notes); run(4000000); capture("notes-while-part-muted",true);
        press(MCU_BUTTON_INST_MUTE,"mute-off");
        press(MCU_BUTTON_INST_ALL,"all-on");
        press(MCU_BUTTON_LEVEL_L,"all-level-down");
        press(MCU_BUTTON_PAN_R,"all-pan-up");
        press(MCU_BUTTON_KEY_SHIFT_R,"all-key-up");
        press(MCU_BUTTON_REVERB_R,"all-reverb-up");
        press(MCU_BUTTON_CHORUS_R,"all-chorus-up");
        press(MCU_BUTTON_INST_MUTE,"all-mute-on");
        emu.PostMIDI(notes); synth.push(notes); run(4000000); capture("notes-while-all-muted",true);
        press(MCU_BUTTON_INST_MUTE,"all-mute-off");
        press(MCU_BUTTON_INST_ALL,"all-off");
        std::puts("Native panel: mute, voice stop, master volume/pan/key and effects levels match H8");
        return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--voice-renderer-clock") == 0) { VerifyVoiceRenderingClock(roms.romset_info,roms.romset); return 0; }
    if(argc == 3 && std::strcmp(argv[2],"--voice-renderer-envelope") == 0) { VerifyVoiceRenderingClock(roms.romset_info,roms.romset,80); return 0; }
    if(argc == 3 && std::strcmp(argv[2],"--voice-renderer-stages") == 0) { VerifyVoiceRenderingClock(roms.romset_info,roms.romset,121,true); return 0; }
    if(argc == 3 && (std::strcmp(argv[2],"--native-reserve-defaults") == 0
        || std::strcmp(argv[2],"--native-controller-work") == 0
        || std::strcmp(argv[2],"--native-capacity-modes") == 0
        || std::strcmp(argv[2],"--native-capacity-stealing") == 0
        || std::strcmp(argv[2],"--native-reserve-sysex") == 0
        || std::strcmp(argv[2],"--native-reserve-stealing") == 0)) {
        const auto& data=roms.romset_info.rom_data;
        const auto& r1=data[size_t(RomLocation::ROM1)];
        const auto& r2=data[size_t(RomLocation::ROM2)];
        sc55::SoundData sounds;
        if(!sounds.loadEncoded(sc55::ImportSoundData(r1,r2))) return 6;
        auto pcm=std::make_unique<pcm_t>(); pcm->is_mk1=true;
        std::copy_n(emu.GetPCM().waverom1,sizeof(pcm->waverom1),pcm->waverom1);
        std::copy_n(emu.GetPCM().waverom2,sizeof(pcm->waverom2),pcm->waverom2);
        std::copy_n(emu.GetPCM().waverom3,sizeof(pcm->waverom3),pcm->waverom3);
        sc55::NativeMelodicPlayer player(sounds,*pcm,sc55::ImportSystemDefaults(r1,r2),
            sc55::ImportRhythmPresets(r1,r2),sc55::ImportMelodicPresets(r1,r2),sc55::ImportEffectsTables(r1,r2));
        const auto end=cpu.cycles+120000000;
        while(cpu.cycles<end) emu.Step();
        for(unsigned part=0;part<16;++part)
            if(player.capacityPolicy().reserves[part]!=MCU_Read(cpu,0x8018+part))
                throw std::runtime_error("Native partial reserve differs from H8 startup");
        if(player.capacityPolicy().startPartControl!=MCU_Read(cpu,0x8028))
            throw std::runtime_error("Native steal priority differs from H8 startup");
        std::puts("Native partial reserve: all 16 defaults and allocation priority match H8 startup");
        if(std::strcmp(argv[2],"--native-capacity-modes")==0) {
            unsigned alternate=0;
            for(unsigned program=0;program<128;++program) {
                const uint8_t packet[]{0xc0,uint8_t(program)};
                emu.PostMIDI(packet); player.push(packet);
                const auto until=cpu.cycles+1000000;
                while(cpu.cycles<until) emu.Step();
                player.renderFrames(1600);
                const auto expected=MCU_Read(cpu,0xa041);
                alternate+=expected==2;
                if(player.failed() || player.capacityPolicy().modes[1]!=expected) {
                    std::printf("Capacity mode program=%u native=%u H8=%u\n",program,
                        player.capacityPolicy().modes[1],expected);
                    throw std::runtime_error("Native program selection omitted H8 capacity mode");
                }
            }
            std::printf("Capacity mode: all 128 programs match H8; alternate=%u\n",alternate);
            return 0;
        }
        if(std::strcmp(argv[2],"--native-reserve-stealing")==0
            || std::strcmp(argv[2],"--native-controller-work")==0
            || std::strcmp(argv[2],"--native-capacity-stealing")==0) {
            if(std::getenv("SC55_CAPACITY_ALIGN_PCM")) {
                player.renderFrames(16384);
                const auto distance=unsigned((pcm->tv_counter-emu.GetPCM().tv_counter)&0x3fff);
                player.renderFrames(distance);
                std::printf("[DEBUG-capacity-eg] aligned idle PCM phases=%04x/%04x\n",
                    emu.GetPCM().tv_counter,pcm->tv_counter);
            }
            std::array<uint16_t,24> controlBases{};
            const bool traceEg=std::getenv("SC55_TRACE_CAPACITY_EG")!=nullptr;
            const bool traceWork=std::getenv("SC55_TRACE_CAPACITY_WORK")!=nullptr;
            const bool controllerWorkOnly=std::strcmp(argv[2],"--native-controller-work")==0;
            const bool traceRoutines=controllerWorkOnly || std::getenv("SC55_TRACE_CONTROL_ROUTINES")!=nullptr;
            ControlWorkProbe routineWork;
            KernelEventProbe kernelEvents;
            if(std::strcmp(argv[2],"--native-capacity-stealing")==0 && std::getenv("SC55_TRACE_KERNEL_EVENTS"))
                activeKernelEvents=&kernelEvents;
            if(traceRoutines) activeControlWork=&routineWork;
            const bool replayPasses=std::getenv("SC55_REPLAY_CAPACITY_PASSES")!=nullptr;
            const bool replayUnitElapsed=replayPasses && std::strcmp(std::getenv("SC55_REPLAY_CAPACITY_PASSES"),"end-unit")==0;
            const bool replayEntryElapsed=replayPasses && std::strcmp(std::getenv("SC55_REPLAY_CAPACITY_PASSES"),"end-clock-start")==0;
            const bool replayNativeElapsed=replayEntryElapsed || (replayPasses && std::strcmp(std::getenv("SC55_REPLAY_CAPACITY_PASSES"),"end-clock")==0);
            const bool replayPassEnd=replayUnitElapsed || replayNativeElapsed || (replayPasses && std::strcmp(std::getenv("SC55_REPLAY_CAPACITY_PASSES"),"end")==0);
            sc55::ControlTaskClock replayElapsedClock;
            std::optional<uint8_t> replayCapturedElapsed;
            const bool replayGroupsAtStart=replayPasses && std::strcmp(std::getenv("SC55_REPLAY_CAPACITY_PASSES"),"groups-start")==0;
            const bool replayHolds=replayPasses && std::strcmp(std::getenv("SC55_REPLAY_CAPACITY_PASSES"),"holds")==0;
            const bool replayGroups=replayHolds || replayGroupsAtStart || (replayPasses && std::strcmp(std::getenv("SC55_REPLAY_CAPACITY_PASSES"),"groups")==0);
            ControlGroupProbe groupProbe;
            groupProbe.captureReadback=replayHolds;
            uint32_t heldChanges=0;
            ControlGroupProbe::Event heldSelection{};
            std::array<uint16_t,24> heldNativeStages{};
            const bool replayIngress=std::getenv("SC55_REPLAY_MIDI_INGRESS")!=nullptr;
            std::optional<sc55::VoiceControlRuntime::ControlStep> groupResult;
#if defined(SC55_NATIVE_IO_AUDIT)
            if(replayPasses) player.useExternalControlClockAudit();
#else
            if(replayPasses) throw std::runtime_error("Control replay requires native IO audit build");
#endif
            bool traceActive=false;
            std::array<uint64_t,2> traceStart{};
            std::array<unsigned,2> traceCount{},traceStage{255,255},traceProgress{65536,65536};
            const auto trace=[&](unsigned side,uint64_t cycles,unsigned stage,unsigned progress,unsigned level,unsigned command,unsigned ticks) {
                if(stage==traceStage[side] && progress==traceProgress[side]) return;
                if(traceCount[side]<12 || stage!=traceStage[side])
                    std::printf("[DEBUG-capacity-time] %s age=%llu stage=%u progress=%u level=%04x command=%04x ticks=%u\n",
                        side?"CPP":"H8",(unsigned long long)(cycles-traceStart[side]),stage,progress,level,command,ticks);
                ++traceCount[side]; traceStage[side]=stage; traceProgress[side]=progress;
            };
            const auto send=[&](std::span<const uint8_t> packet) {
                if(traceEg && packet.size()==3 && packet[0]==0x91 && packet[1]==66) {
                    traceActive=true; traceStart={cpu.cycles,pcm->cycles};
                }
                emu.PostMIDI(packet);
                if(!replayIngress) player.push(packet);
                const auto windowStart=cpu.cycles;
                const auto until=windowStart+4000000;
                const bool traceReuseScan=std::getenv("SC55_TRACE_REUSE_SCAN") && packet.size()==3 && packet[0]==0x92 && packet[1]==57;
                const auto* admissionMode=std::getenv("SC55_REPLAY_ADMISSION");
                const bool delayAdmission=admissionMode && packet.size()==3 && packet[0]==0x92 && packet[1]==57;
                std::optional<unsigned> admissionFrame;
                uint32_t oldScanState=~0u;
                unsigned oldGainReady=2;
                std::array<uint16_t,3> previousFrc{};
                std::optional<uint64_t> firstKernelTick;
                if(traceReuseScan) {
                    std::fprintf(stderr,"[DEBUG-reuse-epoch] H8cycles=%llu CPPcycles=%llu CPPphase=%llu\n",
                        (unsigned long long)cpu.cycles,(unsigned long long)pcm->cycles,
                        (unsigned long long)(pcm->cycles%sc55::ControlTaskClock::kernelTickCycles));
                    for(unsigned i=0;i<3;++i) {
                        const auto& timer=cpu.timer->frt[i]; previousFrc[i]=timer.frc;
                        std::fprintf(stderr,"[DEBUG-reuse-timer] index=%u frc=%u ocra=%u tcr=%u\n",i,timer.frc,timer.ocra,timer.tcr);
                    }
                }
                std::vector<std::pair<unsigned,uint8_t>> controlPasses;
                std::vector<unsigned> controlEntries;
                if(replayPasses) controlScheduleInstruction=[&](const mcu_t& source) {
                    if(&source!=&cpu || source.cp!=0) return;
                    const auto frame=unsigned((source.cycles-windowStart+624)/625);
                    if(source.pc==(replayPassEnd ? 0x5b70 : 0x5af9))
                        controlPasses.emplace_back(frame,uint8_t(MCU_Read16(cpu,0xac5a)));
                    if(replayEntryElapsed && source.pc==0x5af9) controlEntries.push_back(frame);
                };
                struct ClearControlSchedule {
                    ~ClearControlSchedule() {controlScheduleInstruction={};}
                } clearControlSchedule;
                std::vector<ControlGroupProbe::Event> controlGroups;
                // Diagnostic only: separate UART/ISR ingress latency from
                // control ownership. Observe the actual receive-ring commit,
                // without changing H8 execution or adding product delays.
                std::vector<std::pair<unsigned,uint8_t>> ingress;
                std::array<uint64_t,10> work{};
                uint64_t passStart=0,voiceStart=0,passCycles=0,voiceCycles=0,sleepCycles=0;
                uint64_t currentOwnCycles=0,ownPassCycles=0;
                unsigned passes=0;
                while(cpu.cycles<until) {
                    if(delayAdmission && !admissionFrame) {
                        const bool reached=std::strcmp(admissionMode,"entry")==0
                            ? cpu.cp==0 && cpu.pc==0x0bc1
                            : MCU_Read16(cpu,MCU_Read16(cpu,0x677a))==18;
                        if(reached) admissionFrame=unsigned((cpu.cycles-windowStart+624)/625);
                    }
                    if(traceReuseScan && cpu.cycles-windowStart<400*625) {
                        for(unsigned i=0;i<3;++i) {
                            const auto current=cpu.timer->frt[i].frc;
                            if(current<previousFrc[i]) {
                                if(i==1 && !firstKernelTick) firstKernelTick=cpu.cycles-windowStart;
                                std::fprintf(stderr,"[DEBUG-reuse-tick] H8 frame=%.3f timer=%u\n",double(cpu.cycles-windowStart)/625,i);
                            }
                            previousFrc[i]=current;
                        }
                        if(cpu.cp==0 && (cpu.pc==0x2178 || cpu.pc==0x0bc1 || cpu.pc==0x5710 || cpu.pc==0x573e))
                            std::fprintf(stderr,"[DEBUG-admission-phase] H8 frame=%.3f pc=%04x task=%u\n",
                                double(cpu.cycles-windowStart)/625,cpu.pc,MCU_Read16(cpu,0xfdca));
                        const auto base=MCU_Read16(cpu,0x677a); // physical slot8
                        const auto stage=MCU_Read16(cpu,base);
                        const auto visited=MCU_Read(cpu,uint16_t(base-26));
                        const auto combined=uint32_t(stage)|(uint32_t(visited)<<16);
                        const auto& chip=emu.GetPCM();
                        const unsigned gainReady=chip.ram2[8][9]==0 || chip.ram2[8][10]==0;
                        if(gainReady!=oldGainReady || (combined!=oldScanState && stage==18)) {
                            std::fprintf(stderr,"[DEBUG-reuse-gain] H8 frame=%.3f task=%u stage=%u ready=%u gains=%04x/%04x\n",
                                double(cpu.cycles-windowStart)/625,MCU_Read16(cpu,0xfdca),stage,gainReady,chip.ram2[8][9],chip.ram2[8][10]);
                            oldGainReady=gainReady;
                        }
                        if(combined!=oldScanState || (cpu.cp==0 && cpu.pc==0x5b19 && cpu.r[1]==8)) {
                            std::fprintf(stderr,"[DEBUG-reuse-scan] H8 frame=%.3f pc=%02x:%04x task=%u stage=%u visited=%u scan=%u\n",
                                double(cpu.cycles-windowStart)/625,cpu.cp,cpu.pc,MCU_Read16(cpu,0xfdca),stage,visited,
                                cpu.cp==0 && cpu.pc==0x5b19 && cpu.r[1]==8);
                            oldScanState=combined;
                        }
                    }
                    if(traceRoutines) routineWork.before(cpu);
                    if(replayGroups) groupProbe.observe(cpu,windowStart,controlGroups);
                    if(traceWork && cpu.cp==0) {
                        if(cpu.pc==0x5af9) { passStart=cpu.cycles; currentOwnCycles=0; }
                        if(cpu.pc==0x5b0b && passStart) voiceStart=cpu.cycles;
                        if(cpu.pc==0x5b70 && passStart && voiceStart) {
                            passCycles+=cpu.cycles-passStart;
                            voiceCycles+=cpu.cycles-voiceStart;
                            ownPassCycles+=currentOwnCycles;
                            ++passes; passStart=voiceStart=0;
                        }
                    }
                    if(cpu.cp==0 && cpu.pc==0x318c) {
                        const auto base=cpu.r[0];
                        const auto slot=unsigned((MCU_Read(cpu,uint16_t(base-2))<<8)|MCU_Read(cpu,uint16_t(base-1)));
                        if(slot<24) controlBases[slot]=base;
                    }
                    if(traceActive && cpu.cp==0 && cpu.pc==0x5855) {
                        const auto base=cpu.r[0];
                        const auto word=[&](int offset) { const auto a=uint16_t(base+offset); return unsigned((MCU_Read(cpu,a)<<8)|MCU_Read(cpu,uint16_t(a+1))); };
                        const auto slot=word(-2);
                        if(slot<24 && MCU_Read(cpu,0xa318+slot)==2
                            && MCU_Read(cpu,0xa2e8+MCU_Read(cpu,0xa330+slot))==66)
                            trace(0,cpu.cycles,word(0),word(8),word(0x1c),word(0x1e),
                                (MCU_Read(cpu,0xac5a)<<8)|MCU_Read(cpu,0xac5b));
                    }
                    const auto before=cpu.cycles;
                    const auto task=(traceWork || traceRoutines) ? unsigned(MCU_Read16(cpu,0xfdca)) : 10;
                    const bool sleeping=cpu.sleep!=0;
                    const bool kernelPc=cpu.cp==0 && cpu.pc<0x752;
                    const auto rxBefore=replayIngress ? MCU_Read16(cpu,0xabf8) : 0;
                    emu.Step();
                    if(replayIngress && MCU_Read16(cpu,0xabf8)!=rxBefore) {
                        if(rxBefore>=512 || MCU_Read16(cpu,0xabf8)!=(rxBefore+1)%512)
                            throw std::runtime_error("Ingress probe saw a reset, not a received byte");
                        ingress.emplace_back(unsigned((cpu.cycles-windowStart+624)/625),MCU_Read(cpu,0xa3f8+rxBefore));
                    }
                    if(traceRoutines) routineWork.after(cpu.cycles-before,task,sleeping,kernelPc);
                    if(traceWork) {
                        if(task<work.size()) work[task]+=cpu.cycles-before;
                        if(sleeping) sleepCycles+=cpu.cycles-before;
                        if(passStart && task==8 && !sleeping && !kernelPc) currentOwnCycles+=cpu.cycles-before;
                    }
                }
                if(replayIngress && (ingress.size()!=packet.size()
                    || !std::equal(ingress.begin(),ingress.end(),packet.begin(),
                        [](const auto& received,uint8_t sent) {return received.second==sent;})))
                    throw std::runtime_error("Ingress probe did not observe the complete input packet");
                unsigned nativeFrame=0;
                std::size_t ingressIndex=0;
#if defined(SC55_NATIVE_IO_AUDIT)
                if(traceReuseScan && std::getenv("SC55_ALIGN_REUSE_TICK")) {
                    if(!firstKernelTick) throw std::runtime_error("No reference kernel tick observed");
                    player.alignActivationTickAudit(*firstKernelTick);
                }
                if(delayAdmission) {
                    if(!admissionFrame) throw std::runtime_error("Admission probe did not observe the reference event");
                    player.holdVoiceAdmissionsAudit(true);
                    std::fprintf(stderr,"[DEBUG-admission-replay] mode=%s frame=%u\n",admissionMode,*admissionFrame);
                }
#else
                if(delayAdmission) throw std::runtime_error("Admission replay requires native IO audit build");
#endif
                const auto renderNativeUntil=[&](unsigned target) {
#if defined(SC55_NATIVE_IO_AUDIT)
                    if(admissionFrame && *admissionFrame<=target) {
                        player.renderFrames(*admissionFrame-nativeFrame);nativeFrame=*admissionFrame;
                        player.holdVoiceAdmissionsAudit(false);admissionFrame.reset();
                    }
#endif
                    player.renderFrames(target-nativeFrame);nativeFrame=target;
                };
                std::size_t controlEntryIndex=0;
                const auto advanceNative=[&](unsigned count) {
                    const auto target=nativeFrame+count;
                    if(replayNativeElapsed) {
                        auto clockFrame=nativeFrame;
                        while(controlEntryIndex<controlEntries.size() && controlEntries[controlEntryIndex]<=target) {
                            const auto entry=controlEntries[controlEntryIndex++];
                            replayElapsedClock.advance(uint64_t(entry-clockFrame)*625);clockFrame=entry;
                            // Capture at actual entry, before work, not at
                            // its eventual end. Empty means no native event.
                            if(const auto pending=replayElapsedClock.consume()) {
                                if(replayCapturedElapsed) throw std::runtime_error("Overlapping observed control entries");
                                replayCapturedElapsed=pending;
                            }
                        }
                        replayElapsedClock.advance(uint64_t(target-clockFrame)*625);
                    }
                    while(ingressIndex<ingress.size() && ingress[ingressIndex].first<=target) {
                        const auto [frame,byte]=ingress[ingressIndex++];
                        renderNativeUntil(frame);
                        if(player.push(std::span(&byte,1))!=1)
                            throw std::runtime_error("Native ingress replay backpressure");
                    }
                    renderNativeUntil(target);
                };
                if(replayIngress && traceReuseScan) for(auto [frame,byte]:ingress)
                    std::fprintf(stderr,"[DEBUG-ingress] frame=%u byte=%02x\n",frame,byte);
                if(traceWork) {
                    std::printf("[DEBUG-capacity-work] midi=%02x/%02x voices=%u passes=%u meanPass=%llu meanVoices=%llu meanTask8NonKernel=%llu sleep=%llu tasks=",
                        packet[0],packet.size()>1 ? packet[1] : 0,24-unsigned(MCU_Read(cpu,0xa3c1)),passes,
                        (unsigned long long)(passes ? passCycles/passes : 0),
                        (unsigned long long)(passes ? voiceCycles/passes : 0),
                        (unsigned long long)(passes ? ownPassCycles/passes : 0),(unsigned long long)sleepCycles);
                    for(auto cycles:work) std::printf(" %llu",(unsigned long long)cycles);
                    std::puts("");
                }
                if(replayGroups) {
#if defined(SC55_NATIVE_IO_AUDIT)
                    unsigned frame=0,groups=0,passes=0;
                    uint64_t oldNativeState=~uint64_t(0);
                    const auto inspectNative=[&] {
                        const auto* voice=player.voiceControlAudit(8);
                        const auto stage=voice ? voice->lifecycle.stages[0] : 65535u;
                        const auto startup=player.startupAudit();
                        const bool gainReady=pcm->ram2[8][9]==0 || pcm->ram2[8][10]==0;
                        const auto combined=uint64_t(stage)|(uint64_t(startup.status)<<16)|(uint64_t(startup.channels)<<24)
                            |(uint64_t(gainReady)<<56);
                        if(combined!=oldNativeState) {
                            std::fprintf(stderr,"[DEBUG-reuse-scan] CPP frame=%u stage=%u startup=%u reserved=%06x ready=%u gains=%04x/%04x\n",
                                frame,stage,unsigned(startup.status),startup.channels,unsigned(gainReady),pcm->ram2[8][9],pcm->ram2[8][10]);
                            oldNativeState=combined;
                        }
                    };
                    for(const auto& event:controlGroups) {
                        if(event.kind==ControlGroupProbe::Kind::groupBegin && !replayGroupsAtStart && !replayHolds) continue;
                        if(traceReuseScan) {
                            inspectNative();
                            while(frame<event.frame) { advanceNative(1); ++frame; inspectNative(); }
                        } else { advanceNative(event.frame-frame); frame=event.frame; }
                        if(event.kind==ControlGroupProbe::Kind::readback) {
                            const auto result=player.readControlVoiceAudit(event.slot);
                            if(result.status!=sc55::VoiceControlRuntime::ControlProgress::advancedPhase) {
                                const auto position=player.controlPositionAudit();
                                const auto startup=player.startupAudit();
                                std::fprintf(stderr,"[DEBUG-control-hold] startup=%u reserved=%06x failed=%u\n",
                                    unsigned(startup.status),startup.channels,unsigned(player.failed()));
                                for(unsigned slot=0;slot<24;++slot) if(startup.channels&(1u<<slot)) {
                                    const auto* native=player.voiceControlAudit(slot);
                                    std::fprintf(stderr,"[DEBUG-control-hold] reserved slot=%u H8stage=%u nativeStage=%u H8key=%u H8part=%u\n",
                                        slot,event.voices[slot].stage,native ? native->lifecycle.stages[0] : 65535u,
                                        event.voices[slot].key,event.voices[slot].part);
                                }
                                std::fprintf(stderr,"[DEBUG-control-hold] readback mismatch midi=%02x/%02x frame=%u slot=%u nativeSlot=%u phase=%u status=%u\n",
                                    packet[0],packet.size()>1 ? packet[1] : 0,frame,event.slot,
                                    unsigned(position.second.value_or(255)),unsigned(position.first),unsigned(result.status));
                                for(const auto slot:{event.slot,position.second.value_or(255)}) if(slot<24)
                                    std::fprintf(stderr,"[DEBUG-control-hold] selection frame=%u slot=%u stage=%u/%u key=%u\n",
                                        heldSelection.frame,unsigned(slot),heldSelection.voices[slot].stage,
                                        heldNativeStages[slot],heldSelection.voices[slot].key);
                                throw std::runtime_error("Native/H8 control readback differs");
                            }
                            heldChanges|=result.changedMask.lowWord();
                            continue;
                        }
                        if(event.kind==ControlGroupProbe::Kind::groupBegin) {
                            if(replayHolds) {
                                heldSelection=event;
                                for(unsigned slot=0;slot<24;++slot) {
                                    const auto* voice=player.voiceControlAudit(slot);
                                    heldNativeStages[slot]=voice ? voice->lifecycle.stages[0] : 65535;
                                }
                                if(player.selectControlGroupAudit().status!=sc55::VoiceControlRuntime::ControlProgress::advancedPhase)
                                    throw std::runtime_error("Native/H8 control selection differs");
                                continue;
                            }
                            if(groupResult) throw std::runtime_error("Overlapping H8 control groups");
                            groupResult=player.resumeControlGroupAudit();
                            continue;
                        }
                        if(event.kind==ControlGroupProbe::Kind::begin) {
                            if(!player.beginControlGroupsAudit(event.ticks)) {
                                std::fprintf(stderr,"[DEBUG-group-replay] native busy at H8 begin midi=%02x/%02x frame=%u\n",
                                    packet[0],packet.size()>1 ? packet[1] : 0,frame);
                                throw std::runtime_error("Cannot align native control-group begin");
                            }
                            ++passes;
                        } else {
                            const bool fromStart=replayGroupsAtStart && event.kind==ControlGroupProbe::Kind::group;
                            if(fromStart && !groupResult) throw std::runtime_error("H8 control group has no captured start");
                            auto result=fromStart ? *groupResult : player.resumeControlGroupAudit();
                            if(replayHolds) {result.changedMask|=heldChanges;heldChanges=0;}
                            if(fromStart) groupResult.reset();
                            const auto expected=event.kind==ControlGroupProbe::Kind::end
                                ? sc55::VoiceControlRuntime::ControlProgress::complete
                                : sc55::VoiceControlRuntime::ControlProgress::updatedGroup;
                            if(result.status!=expected || (event.kind==ControlGroupProbe::Kind::group && result.changedMask!=event.updated)) {
                                const auto startup=player.startupAudit();
                                const auto& allocation=player.allocatorAudit();
                                for(unsigned slot=0;slot<24;++slot) if((event.updated|result.changedMask)&(1u<<slot)) {
                                    const auto& h8=event.voices[slot];
                                    const auto* native=player.voiceControlAudit(slot);
                                    const auto group=allocation.allocations[slot].noteGroup;
                                    std::fprintf(stderr,"[DEBUG-group-selection] slot=%u stage=%u/%u part=%u/%u key=%u/%u activity=%u/%u\n",
                                        slot,h8.stage,native ? native->lifecycle.stages[0] : 65535u,
                                        h8.part,allocation.allocations[slot].part,h8.key,group<24 ? allocation.noteGroups[group].key : 255u,
                                        h8.activity,allocation.activity[slot]);
                                }
                                std::fprintf(stderr,"[DEBUG-group-startup] status=%u channels=%06x keys=%06x/%06x queue=%zu\n",
                                    unsigned(startup.status),startup.channels,pcm->voice_mask,pcm->voice_mask_pending,player.queuedEvents());
                                for(unsigned slot=0;slot<24;++slot) if(startup.channels&(1u<<slot))
                                    std::fprintf(stderr,"[DEBUG-group-startup] slot=%u age=%llu mode=%04x gains=%04x/%04x commands=%04x/%04x\n",
                                        slot,(unsigned long long)(pcm->cycles-pcm->native_voice_install_cycle[slot]),
                                        pcm->ram2[slot][7],pcm->ram2[slot][9],pcm->ram2[slot][10],pcm->ram2[slot][3],pcm->ram2[slot][4]);
                                std::fprintf(stderr,"[DEBUG-group-replay] traversal mismatch midi=%02x/%02x frame=%u kind=%u status=%u masks=%06x/%06x\n",
                                    packet[0],packet.size()>1 ? packet[1] : 0,frame,unsigned(event.kind),unsigned(result.status),event.updated,result.changedMask.lowWord());
                                throw std::runtime_error("Native/H8 control group traversal differs");
                            }
                            if(event.kind==ControlGroupProbe::Kind::group) ++groups;
                        }
                    }
                    advanceNative(6400-frame);
                    std::printf("[DEBUG-group-replay] midi=%02x/%02x passes=%u groups=%u\n",
                        packet[0],packet.size()>1 ? packet[1] : 0,passes,groups);
#endif
                }
                else if(replayPasses) {
#if defined(SC55_NATIVE_IO_AUDIT)
                    unsigned frame=0,totalTicks=0,coalesced=0,emptyClock=0;
                    for(auto [target,ticks]:controlPasses) {
                        advanceNative(target-frame); frame=target;
                        // Counterfactual diagnostic: preserve the exact end
                        // schedule, changing only the elapsed-count input.
                        // Never use this schedule or unit count in production.
                        auto elapsed=replayUnitElapsed ? uint8_t(1) : ticks;
                        if(replayNativeElapsed) {
                            const auto pending=replayEntryElapsed ? replayCapturedElapsed : replayElapsedClock.consume();
                            if(replayEntryElapsed) replayCapturedElapsed.reset();
                            // The native clock has its own startup epoch. An
                            // observed opportunity does not manufacture an
                            // expiration (zero is a wrapped256, not no work).
                            if(!pending) {++emptyClock;continue;}
                            elapsed=*pending;
                        }
                        if(!player.signalControlPassAudit(elapsed)) ++coalesced;
                        totalTicks+=elapsed;
                    }
                    advanceNative(6400-frame);
                    std::printf("[DEBUG-capacity-replay] midi=%02x/%02x passes=%zu ticks=%u coalesced=%u emptyClock=%u\n",
                        packet[0],packet.size()>1 ? packet[1] : 0,controlPasses.size(),totalTicks,coalesced,emptyClock);
#endif
                }
                else if(!traceEg) advanceNative(6400);
                else for(unsigned frame=0;frame<6400;++frame) {
                    if(replayIngress) advanceNative(1);
                    else player.step();
#if defined(SC55_NATIVE_IO_AUDIT)
                    if(traceActive) {
                        const auto& allocation=player.allocatorAudit();
                        for(unsigned slot=0;slot<24;++slot)
                            if(!(allocation.allocations[slot].status&128) && allocation.allocations[slot].part==2
                                && allocation.noteGroups[allocation.allocations[slot].noteGroup].key==66)
                                if(const auto* voice=player.voiceControlAudit(slot)) {
                                    const auto& e=voice->amplitude.state();
                                    trace(1,pcm->cycles,unsigned(e.segment.stage)*2,e.segment.progress.position,e.level,e.pcmWord,0);
                                }
                    }
#endif
                }
                if(player.failed()) throw std::runtime_error("Native player failed during reserve stealing");
            };
            if(controllerWorkOnly) {
                const uint8_t program[]{0xc0,48},note[]{0x90,60,100};
                send(program);send(note);
                for(uint8_t setting:{uint8_t(0),uint8_t(127)}) {
                    for(unsigned source=0;source<6;++source) {
                        std::vector<uint8_t> packet{0xf0,0x41,0x10,0x42,0x12,0x40,0x21,uint8_t(source*16)};
                        unsigned checksum=0x40+0x21+source*16;
                        for(unsigned column=0;column<11;++column) {packet.push_back(setting);checksum+=setting;}
                        packet.push_back(uint8_t(-checksum&127));packet.push_back(0xf7);send(packet);
                    }
                    for(uint8_t value:{uint8_t(0),uint8_t(64),uint8_t(127)}) {
                        const uint8_t pressure[]{0xa0,60,value},channelPressure[]{0xd0,value};
                        const uint8_t bend[]{0xe0,value,value},mod[]{0xb0,1,value};
                        const uint8_t cc1[]{0xb0,16,value},cc2[]{0xb0,17,value};
                        send(pressure);send(channelPressure);send(bend);send(mod);send(cc1);send(cc2);
                    }
                }
                // Exercise release as well as held-note controller changes.
                // All traffic enters through MIDI; never patch H8 stage RAM.
                const uint8_t off[]{0x80,60,64};
                for(uint8_t value:{uint8_t(0),uint8_t(64),uint8_t(127)}) {
                    const uint8_t release[]{0xb0,72,value};
                    send(release);send(off);send(note);
                }
                send(off);
                // Exercise both signs of moving portamento through real MIDI.
                // The semantic pitch comparison must not pass on held pitches alone.
                const uint8_t glideOn[]{0xb0,65,127},glideTime[]{0xb0,5,100};
                send(glideTime);send(glideOn);
                for(uint8_t key:{uint8_t(48),uint8_t(84),uint8_t(36),uint8_t(72)}) {
                    const uint8_t on[]{0x90,key,100},release[]{0x80,key,64};
                    send(on);send(release);
                }
                const uint8_t glideOff[]{0xb0,65,0};
                send(glideOff);
                // Output-control coverage must include real pan/send motion,
                // silence and the drum tone scales, not just steady defaults.
                send(note);
                for(uint8_t value:{uint8_t(0),uint8_t(64),uint8_t(127)})
                    for(uint8_t controller:{uint8_t(7),uint8_t(11),uint8_t(10),uint8_t(91),uint8_t(93)}) {
                        const uint8_t cc[]{0xb0,controller,value};send(cc);
                    }
                send(off);
                // CC10=0 is stored as1 by the SC-55. GS part pan0 is the
                // distinct random/frozen-pan setting; use its real receiver.
                const uint8_t randomPan[]{0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0x1c,0,0x13,0xf7};
                send(randomPan);send(note);send(off);
                for(uint8_t key:{uint8_t(49),uint8_t(51),uint8_t(41)}) {
                    const uint8_t drum[]{0x99,key,100};send(drum);
                }
                // Select real capital tones containing as-yet unseen LFO
                // shapes. This avoids guessing programs or patching H8 RAM.
                for(unsigned program=0;program<128;++program) {
                    const auto* patch=sounds.patch(sc55::v121CapitalToneIndices[program]);
                    bool needed=false;
                    if(patch) for(const auto& partial:patch->partial) if(partial.used)
                        for(unsigned mode:{unsigned(partial.raw[4]),unsigned(patch->common[2])}) {
                            const unsigned selector=mode&15;
                            const unsigned shape=selector<=3 ? selector : selector>=8 && selector<=10 ? selector-4 : 0;
                            needed|=routineWork.modulationControl.shapes[shape]==0;
                        }
                    if(needed) {
                        const uint8_t select[]{0xc0,uint8_t(program)};
                        send(select);send(note);send(off);
                    }
                }
                // Shared second-LFO coverage needs overlapping instances of
                // a tone whose actual mode enables sharing (bit4).
                for(unsigned program=0;program<128;++program) {
                    const auto* patch=sounds.patch(sc55::v121CapitalToneIndices[program]);
                    bool shared=patch && (patch->common[2]&16)
                        && (!routineWork.firstModulationRouting.checks[1] || !routineWork.firstModulationRouting.checks[2]);
                    if(patch) for(const auto& partial:patch->partial)
                        shared|=partial.used && (partial.raw[4]&16)
                            && (!routineWork.modulationRouting.checks[1] || !routineWork.modulationRouting.checks[2]);
                    if(!shared) continue;
                    const uint8_t select[]{0xc0,uint8_t(program)},release[]{0xb0,72,0};
                    const bool firstShared=(patch->common[2]&16)!=0;
                    const uint8_t note2[]{0x90,uint8_t(firstShared ? 64 : 60),100};
                    const uint8_t note3[]{0x90,uint8_t(firstShared ? 67 : 60),100};
                    const uint8_t off2[]{0x80,note2[1],64},off3[]{0x80,note3[1],64};
                    send(select);send(release);send(note);send(note2);send(note3);send(off);
                    for(unsigned tick=0;tick<8;++tick) {const uint8_t cc[]{0xb0,1,uint8_t(tick)};send(cc);}
                    send(off2);send(off3);
                    if(routineWork.modulationRouting.checks[1] && routineWork.modulationRouting.checks[2]
                        && routineWork.firstModulationRouting.checks[1] && routineWork.firstModulationRouting.checks[2]) break;
                }
                routineWork.report();
                if(routineWork.controllerBudgets.size()<3)
                    throw std::runtime_error("Controller work test did not exercise variable calculation paths");
                if(routineWork.conversionBudgets.size()<3 || routineWork.outputBudgets.size()<3)
                    throw std::runtime_error("Envelope work test did not exercise variable calculation paths");
                if(routineWork.filterBudgets.size()<3 || !routineWork.filterStages[6])
                    throw std::runtime_error("Filter work test did not exercise variable/release calculation paths");
                if(!routineWork.pitchControl.glideDirections[0] || !routineWork.pitchControl.glideDirections[1])
                    throw std::runtime_error("Pitch test did not exercise both portamento directions");
                if(!routineWork.voiceOutput.frozen || !routineWork.voiceOutput.scaled
                    || !routineWork.voiceOutput.movingPan || !routineWork.voiceOutput.movingSends)
                    throw std::runtime_error("Output test did not exercise pan/send motion and tone scales");
                if(!routineWork.amplitudeControl.ends || !routineWork.amplitudeControl.stages[12])
                    throw std::runtime_error("Amplitude test did not exercise release and natural completion");
                return 0;
            }
            std::vector<uint8_t> reserve{0xf0,0x41,0x10,0x42,0x12,0x40,1,0x10};
            unsigned sum=0x51;
            for(unsigned part=0;part<16;++part) {
                const uint8_t value=part>=1 && part<=3 ? 8 : 0;
                reserve.push_back(value); sum+=value;
            }
            reserve.push_back(uint8_t((-sum)&127)); reserve.push_back(0xf7); send(reserve);
            for(uint8_t channel=0;channel<3;++channel) {
                const uint8_t program[]{uint8_t(0xc0|channel),
                    uint8_t(std::strcmp(argv[2],"--native-capacity-stealing")==0 ? 0 : 80)};
                send(program);
            }
            for(unsigned note=0;note<48;++note) {
#if defined(SC55_NATIVE_IO_AUDIT)
                if(note==40) {
                    if(activeKernelEvents) {kernelEvents.report(cpu);activeKernelEvents=nullptr;}
                    if(traceRoutines) routineWork.report();
                    const auto& native=player.allocatorAudit();
                    for(unsigned slot=0;slot<24;++slot) {
                        if(MCU_Read(cpu,0xa318+slot)==2 && !(MCU_Read(cpu,0xa348+slot)&128))
                            std::printf("Before steal H8 slot=%u key=%u activity=%u\n",slot,
                                MCU_Read(cpu,0xa2e8+MCU_Read(cpu,0xa330+slot)),MCU_Read(cpu,0xac42+slot));
                        if(native.allocations[slot].part==2 && !(native.allocations[slot].status&128))
                            std::printf("Before steal native slot=%u key=%u activity=%u\n",slot,
                                native.noteGroups[native.allocations[slot].noteGroup].key,native.activity[slot]);
                        if(slot==4 || slot==5) {
                            const auto& h=emu.GetPCM();
                            if(const auto* state=player.voiceControlAudit(slot)) {
                                const auto base=controlBases[slot];
                                const auto word=[&](unsigned offset) { return unsigned((MCU_Read(cpu,base+offset)<<8)|MCU_Read(cpu,base+offset+1)); };
                                const auto& e=state->amplitude.state();
                                std::printf("[DEBUG-capacity-eg] slot=%u base=%04x stage=%u/%u progress=%u/%u deferred=%u/%u logical=%04x/%04x command=%04x/%04x\n",
                                    slot,base,word(0),unsigned(e.segment.stage)*2,word(8),e.segment.progress.position,
                                    word(0x12),e.segment.progress.deferredTicks,word(0x1c),e.level,word(0x1e),e.pcmWord);
                            }
                            std::printf("[DEBUG-capacity-eg] slot=%u command H8=%04x,%04x,%04x native=%04x,%04x,%04x level H8=%04x,%04x,%04x native=%04x,%04x,%04x clock=%04x/%04x\n",
                                slot,h.ram2[slot][3],h.ram2[slot][4],h.ram2[slot][5],pcm->ram2[slot][3],pcm->ram2[slot][4],pcm->ram2[slot][5],
                                h.ram2[slot][9],h.ram2[slot][10],h.ram2[slot][11],pcm->ram2[slot][9],pcm->ram2[slot][10],pcm->ram2[slot][11],h.tv_counter,pcm->tv_counter);
                        }
                    }
                }
#endif
                const uint8_t channel=uint8_t(note<36 ? note/12 : note%3);
                const uint8_t packet[]{uint8_t(0x90|channel),uint8_t(48+note%24),100}; send(packet);
                bool match=true;
                for(unsigned part=0;part<16;++part)
                    match &= player.partVoiceCount(part)==MCU_Read(cpu,0xa1f0+part);
                for(unsigned part=1;part<=3;++part) {
                    std::array<uint8_t,128> expected{};
                    for(unsigned slot=0;slot<24;++slot)
                        if(!(MCU_Read(cpu,0xa348+slot)&128) && MCU_Read(cpu,0xa318+slot)==part) {
                            const auto group=MCU_Read(cpu,0xa330+slot);
                            if(group>=24) throw std::runtime_error("Invalid H8 voice group");
                            const auto key=MCU_Read(cpu,0xa2e8+group);
                            if(key<128) ++expected[key];
                        }
                    const auto actual=player.partNoteVoices(part);
                    if(expected!=actual)
                        for(unsigned key=0;key<128;++key) if(expected[key]!=actual[key])
                            std::printf("Survivor part=%u key=%u H8=%u native=%u\n",part,key,expected[key],actual[key]);
                    match &= expected==actual;
                }
                if(!match) {
                    std::printf("Stealing mismatch note=%u channel=%u H8/native:",note,channel);
                    for(unsigned part=0;part<4;++part)
                        std::printf(" %u/%u",MCU_Read(cpu,0xa1f0+part),player.partVoiceCount(part));
                    std::puts(""); std::fflush(stdout);
                    throw std::runtime_error("Native reserved voice allocation differs from H8");
                }
            }
            std::puts("Partial reserve: 48 note admissions across three parts match H8 voice counts and surviving notes");
            for(uint8_t channel=0;channel<3;++channel) {
                const uint8_t off[]{uint8_t(0xb0|channel),120,0}; send(off);
            }
            for(unsigned part=0;part<16;++part) reserve[8+part]=part==1 ? 24 : 0;
            send(reserve); // Same total24, therefore same checksum.
            for(uint8_t note=48;note<60;++note) {
                const uint8_t packet[]{0x90,note,100}; send(packet);
            }
            const uint8_t mono[]{0xb1,126,0}; send(mono);
            const uint8_t blocked[]{0x91,72,100}; send(blocked);
            for(unsigned part=0;part<16;++part)
                if(player.partVoiceCount(part)!=MCU_Read(cpu,0xa1f0+part))
                    throw std::runtime_error("Full reserve rejection differs from H8");
            std::puts("Partial reserve: fully protected capacity rejects an incoming mono note without engine failure");
            const uint8_t freeProtected[]{0xb0,120,0}; send(freeProtected);
            const uint8_t resumed[]{0x91,73,100}; send(resumed);
            if(player.partVoiceCount(2)==0 || player.queuedEvents()!=0)
                throw std::runtime_error("Rejected mono note prevented subsequent playback");
            for(unsigned part=0;part<16;++part)
                if(player.partVoiceCount(part)!=MCU_Read(cpu,0xa1f0+part))
                    throw std::runtime_error("Playback after reserve rejection differs from H8");
        }
        if(std::strcmp(argv[2],"--native-reserve-sysex")==0) {
            std::vector<std::pair<uint8_t,std::vector<uint8_t>>> cases{
                {0x10,std::vector<uint8_t>(16,0)}, {0x10,{24}},
                {0x10,{0,8,8,8,0,0,0,0,0,0,0,0,0,0,0,0}},
                {0x10,std::vector<uint8_t>(16,2)}, {0x11,std::vector<uint8_t>(16,0)},
                {0x20,{12}}, {0x20,{127}}, {0x20,{5,6}}, {0x20,{}},
                {0x10,std::vector<uint8_t>(17,0)}, {0x10,std::vector<uint8_t>(15,0)},
                {0x10,{0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}};
            unsigned differences=0;
            for(const auto& [address,values]:cases) {
                std::vector<uint8_t> packet{0xf0,0x41,0x10,0x42,0x12,0x40,1,address};
                unsigned sum=0x41+address;
                for(auto value:values) { packet.push_back(value); sum+=value; }
                packet.push_back(uint8_t((-sum)&127)); packet.push_back(0xf7);
                emu.PostMIDI(packet); player.push(packet);
                const auto until=cpu.cycles+4000000;
                while(cpu.cycles<until) emu.Step();
                player.renderFrames(6400);
                std::printf("Reserve message %02x length=%zu H8:",address,values.size());
                for(unsigned part=0;part<16;++part) {
                    const auto value=MCU_Read(cpu,0x8018+part); std::printf(" %u",value);
                    differences+=value!=player.capacityPolicy().reserves[part];
                }
                const auto priority=MCU_Read(cpu,0x8028);
                differences+=priority!=player.capacityPolicy().startPartControl;
                std::printf(" priority=%u\n",priority);
            }
            if(differences) throw std::runtime_error("Native reserve SysEx differs from H8");
        }
        return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--inactive-voice-equivalence") == 0) {
        auto fast=std::make_unique<pcm_t>(emu.GetPCM());
        auto reference=std::make_unique<pcm_t>(*fast);
        fast->output_sample=reference->output_sample=nullptr;
        fast->output_irq=reference->output_irq=nullptr;
        fast->config.reg_slots=reference->config.reg_slots=24;
        fast->is_mk1=reference->is_mk1=true;
        fast->use_simulation=reference->use_simulation=false;
        fast->skip_inactive_voices=true; reference->skip_inactive_voices=false;
        uint32_t random=7163;
        auto next=[&] { random=random*1664525u+1013904223u; return random; };
        for(unsigned frame=0;frame<8192;++frame) {
            if(frame%32==0) {
                const auto mask=frame%64 ? next()&0xffffff : 0;
                fast->voice_mask=reference->voice_mask=mask;
                fast->voice_mask_pending=reference->voice_mask_pending=mask;
                fast->nfs=reference->nfs=bool(frame&64);
                fast->irq_assert=reference->irq_assert=false;
                for(unsigned slot=0;slot<32;++slot) {
                    for(unsigned i=0;i<8;++i) fast->ram1[slot][i]=reference->ram1[slot][i]=next()&0xfffff;
                    for(unsigned i=0;i<16;++i) fast->ram2[slot][i]=reference->ram2[slot][i]=uint16_t(next()>>8);
                }
            }
            PCM_Update(*fast,fast->cycles+625);
            PCM_Update(*reference,reference->cycles+625);
            if(std::memcmp(fast->ram1,reference->ram1,sizeof(fast->ram1))
                || std::memcmp(fast->ram2,reference->ram2,sizeof(fast->ram2))
                || std::memcmp(fast->eram,reference->eram,sizeof(fast->eram))
                || fast->accum_l!=reference->accum_l || fast->accum_r!=reference->accum_r
                || fast->rcsum[0]!=reference->rcsum[0] || fast->rcsum[1]!=reference->rcsum[1]
                || fast->cycles!=reference->cycles || fast->tv_counter!=reference->tv_counter
                || fast->irq_assert!=reference->irq_assert || fast->irq_channel!=reference->irq_channel)
                throw std::runtime_error("Inactive voice bypass differs from full PCM pipeline");
        }
        std::puts("Inactive PCM: 8192 randomized mixed-key frames match voice RAM, effect memory, buses, IRQ and clock");
        return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--profile-native-idle") == 0) {
        const auto& data=roms.romset_info.rom_data;
        const auto bytes=sc55::ImportSoundData(data[size_t(RomLocation::ROM1)],data[size_t(RomLocation::ROM2)]);
        sc55::NativeSynth synth(bytes,data[size_t(RomLocation::ROM1)],data[size_t(RomLocation::ROM2)],
            data[size_t(RomLocation::WAVEROM1)],data[size_t(RomLocation::WAVEROM2)],
            data[size_t(RomLocation::WAVEROM3)]);
        std::array<AudioFrame<int32_t>,256> frames{};
        for(unsigned i=0;i<1250;++i) synth.render(frames);
        const auto start=std::chrono::steady_clock::now();
        for(unsigned i=0;i<125000;++i) synth.render(frames);
        if(synth.failed()) throw std::runtime_error("Idle profiling failed");
        const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        std::printf("C++ controller/reference PCM idle: 1000 audio seconds in %.6f wall seconds\n",elapsed);
        return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--native-synth") == 0) { VerifyNativeSynth(emu,roms.romset_info); return 0; }
    if(argc == 3 && std::strcmp(argv[2],"--native-synth-fast") == 0) { VerifyNativeSynth(emu,roms.romset_info,true); return 0; }
    if(argc == 3 && std::strcmp(argv[2],"--voice-renderer-programs") == 0) { VerifyNativeSynth(emu,roms.romset_info,true,true); return 0; }
    if(argc == 3 && std::strcmp(argv[2],"--voice-renderer-polyphony") == 0) { VerifyNativeSynth(emu,roms.romset_info,true,true,24); return 0; }
    if(argc == 3 && std::strcmp(argv[2],"--product-engine-selection") == 0) {
        const auto* cache=std::getenv("SC55_TEST_CACHE");
        if(!cache) return 6;
        NukedSC55Emulator adapter;
        if(!adapter.initialise(argv[1],44100,cache)) throw std::runtime_error("Engine selection initialization failed");
        const auto* option=std::getenv("NUKED_SC55_USE_H8");
        const bool expectedNative=option==nullptr || std::strcmp(option,"1")!=0;
        if(adapter.getDebugState().nativeEngine!=expectedNative)
            throw std::runtime_error("Wrong product engine selected");
        std::printf("Product engine selection: %s confirmed\n",expectedNative ? "C++" : "H8");
        return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--native-large-block") == 0) {
        const auto* cache=std::getenv("SC55_TEST_CACHE");
        if(!cache) return 6;
        NukedSC55Emulator whole,split;
        if(!whole.initialise(argv[1],44100,cache) || !split.initialise(argv[1],44100,cache))
            throw std::runtime_error("Large block adapter initialization failed");
        constexpr unsigned count=200000;
        std::vector<float> a(count),b(count),ar(count),br(count);
        const uint8_t note[]{0x90,60,100}; whole.sendMidi(note,3); split.sendMidi(note,3);
        whole.render(a.data(),ar.data(),count);
        for(unsigned at=0;at<count;) {
            const unsigned n=std::min(257u,count-at);
            split.render(b.data()+at,br.data()+at,n); at+=n;
        }
        if(a!=b || ar!=br || std::none_of(a.begin(),a.end(),[](float x){return x!=0;}))
            throw std::runtime_error("Large host block changed audio or returned silence");
        std::puts("Native adapter: 200000-frame host block matches 257-frame partitions exactly");
        return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--native-adapter") == 0) {
        const auto* cache=std::getenv("SC55_TEST_CACHE");
        if(!cache) return 6;
        NukedSC55Emulator adapter;
        std::array<float,513> left{},right{};
        for(const double rate:{44100.0,48000.0,96000.0}) {
            if(!adapter.initialise(argv[1],rate,cache)) throw std::runtime_error("Adapter initialization failed");
            for(unsigned i=0;i<128;++i) adapter.render(left.data(),right.data(),513);
            const uint8_t note[]{0x90,60,100}; adapter.sendMidi(note,3);
            float peak=0;
            for(unsigned i=0;i<128;++i) {
                adapter.render(left.data(),right.data(),i%2?1:513);
                for(unsigned j=0;j<(i%2?1u:513u);++j) {
                    if(!std::isfinite(left[j]) || !std::isfinite(right[j])) throw std::runtime_error("Nonfinite output");
                    peak=std::max(peak,std::abs(left[j]));
                }
            }
            if(peak<0.0001f) throw std::runtime_error("Adapter is silent");
            sc55::SynthState state;
            if(!adapter.getNativeState(state)) throw std::runtime_error("Missing native state");
            adapter.render(left.data(),right.data(),513);
            if(!adapter.getNativeState(state) || state.failed || !state.renderedFrames
                || !state.activeVoiceMask || !state.parts[1].voices || state.parts[1].channel!=0)
                throw std::runtime_error("Native state does not reflect sounding MIDI channel 1");
            const uint8_t volume[]{0xb0,7,37}; adapter.sendMidi(volume,3);
            // First render applies MIDI; a requested subsequent snapshot then
            // captures the state after the message, not the previous frame.
            adapter.render(left.data(),right.data(),513);
            adapter.getNativeState(state);
            adapter.render(left.data(),right.data(),513);
            if(!adapter.getNativeState(state) || state.parts[1].volume!=37)
                throw std::runtime_error("Native state missed CC7");
            using Button=NukedSC55Emulator::FrontPanelButton;
            for(const auto button:{Button::levelInc,Button::levelInc,Button::partInc,
                Button::instrumentInc,Button::panDec,Button::midiChannelInc,Button::keyShiftInc})
                adapter.pressFrontPanelButton(button);
            adapter.render(left.data(),right.data(),513);
            adapter.getNativeState(state);
            adapter.render(left.data(),right.data(),513);
            if(!adapter.getNativeState(state) || state.parts[1].volume!=39 || state.selectedPart!=1
                || state.parts[2].program!=1 || state.parts[2].pan!=63 || state.parts[2].channel!=2
                || state.parts[2].keyShift!=65)
                throw std::runtime_error("Native panel edits lost ordering or targeted the wrong part");
            std::vector<uint8_t> mask(LCD_DISPLAY_WIDTH*LCD_DISPLAY_HEIGHT);
            if(!adapter.copyLcdDisplay(mask.data(),LCD_DISPLAY_WIDTH)
                || std::count(mask.begin(),mask.end(),uint8_t(1))<100)
                throw std::runtime_error("Native LCD is blank");
            if(rate==44100) {
                std::ofstream image(std::string(cache)+"/native-panel.pgm",std::ios::binary);
                image<<"P5\n"<<LCD_DISPLAY_WIDTH<<" "<<LCD_DISPLAY_HEIGHT<<"\n255\n";
                for(auto pixel:mask) image.put(char(pixel==1?0:pixel==2?220:255));
            }
            adapter.pressFrontPanelButton(Button::partDec);
            adapter.pressFrontPanelButton(Button::mute);
            for(unsigned i=0;i<16;++i) adapter.render(left.data(),right.data(),513);
            adapter.getNativeState(state); adapter.render(left.data(),right.data(),513);
            if(!adapter.getNativeState(state) || !state.parts[1].muted || state.parts[1].voices)
                throw std::runtime_error("Native panel mute did not stop the selected part");
            adapter.pressFrontPanelButton(Button::all);
            adapter.pressFrontPanelButton(Button::levelDec);
            adapter.pressFrontPanelButton(Button::panInc);
            adapter.pressFrontPanelButton(Button::keyShiftInc);
            adapter.pressFrontPanelButton(Button::reverbInc);
            adapter.pressFrontPanelButton(Button::chorusInc);
            adapter.pressFrontPanelButton(Button::mute);
            adapter.render(left.data(),right.data(),513);
            adapter.getNativeState(state); adapter.render(left.data(),right.data(),513);
            if(!adapter.getNativeState(state) || !state.allSelected || !state.globalMuted || state.masterVolume!=126
                || state.masterPan!=65 || state.masterKeyShift!=65 || state.reverbLevel!=65 || state.chorusLevel!=65)
                throw std::runtime_error("Native ALL/master/mute commands were not connected");
            // Resolve a whole interaction burst before the audio owner runs.
            // Standby must not let an ignored PART press redirect later edits;
            // two SOLO presses must not both read the same stale UI snapshot.
            for(const auto button:{Button::all,Button::standbyOn,Button::fastScrollOn,Button::partInc,
                Button::standbyOff,Button::partInc,Button::levelInc,Button::solo,Button::solo})
                adapter.pressFrontPanelButton(button);
            adapter.render(left.data(),right.data(),513);
            adapter.getNativeState(state); adapter.render(left.data(),right.data(),513);
            if(!adapter.getNativeState(state) || state.allSelected || state.selectedPart!=1
                || state.parts[2].volume!=101 || state.soloEnabled || !adapter.getDebugState().fastDisplayScroll)
                throw std::runtime_error("UI-resolved command burst lost standby, focus or toggle ordering");
            adapter.pressFrontPanelButton(Button::programReceiveOff);
            adapter.render(left.data(),right.data(),513);
            // This fixture moved part2's RX channel from1 to2 above.
            const uint8_t gatedProgram[]{0xc2,20};adapter.sendMidi(gatedProgram,2);
            for(unsigned i=0;i<16;++i) adapter.render(left.data(),right.data(),513);
            adapter.getNativeState(state);adapter.render(left.data(),right.data(),513);
            if(!adapter.getNativeState(state) || state.parts[2].program!=1
                || adapter.getDebugState().receiveProgramChanges || !(adapter.savedMidiInputState()&0x800))
                throw std::runtime_error("Panel Program Change gate was not applied/published/saved");
            adapter.pressFrontPanelButton(Button::programReceiveOn);
            adapter.render(left.data(),right.data(),513);
            adapter.sendMidi(gatedProgram,2);
            // Let an admitted program change finish its existing voice work.
            for(unsigned i=0;i<16;++i) adapter.render(left.data(),right.data(),513);
            adapter.getNativeState(state);adapter.render(left.data(),right.data(),513);
            if(!adapter.getNativeState(state) || state.parts[2].program!=20) {
                std::fprintf(stderr,"Program gate: program=%u channel=%u receive=%u\n",state.parts[2].program,
                    state.parts[2].channel,unsigned(adapter.getDebugState().receiveProgramChanges));
                throw std::runtime_error("Panel Program Change gate did not reenable MIDI");
            }
            std::printf("Native adapter rate=%.0f peak=%.6f passed\n",rate,peak);
            adapter.release();
            if(adapter.getNativeState(state)) throw std::runtime_error("Released native state still active");
        }
        return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--partial-transitions") == 0) { TracePartialTransitions(emu); return 0; }
    if(argc == 3 && std::strcmp(argv[2],"--high-notes") == 0) { TraceHighNotes(emu); return 0; }
    auto word=[&](unsigned p){return MCU_Read16(cpu,p);};
    if(argc == 3 && std::strcmp(argv[2],"--midi-timing") == 0) {
        auto& pcm=emu.GetPCM();
        PCM_UseSimulation(pcm,false);
        pcm.use_float_effects=false;
        const auto bootEnd=cpu.cycles+120000000;
        while(cpu.cycles<bootEnd) emu.Step();
        uint64_t origin=cpu.cycles,lastTick=0;
        uint32_t mask=pcm.voice_mask&pcm.voice_mask_pending;
        bool tracePitch=false;
        uint16_t pitch=pcm.ram2[23][0];
        auto advance=[&](uint64_t duration) {
            const auto end=cpu.cycles+duration;
            while(cpu.cycles<end) {
                if(!cpu.sleep && cpu.cp==0) {
                    if(cpu.pc==0x5af1) {
                        std::printf("TICK at=%llu delta=%llu\n",(unsigned long long)(cpu.cycles-origin),
                            (unsigned long long)(lastTick?cpu.cycles-lastTick:0)); lastTick=cpu.cycles;
                    }
                    if(cpu.pc==0x0bc1 || cpu.pc==0x09f7 || cpu.pc==0x586c || cpu.pc==0x2340)
                        std::printf("PATH at=%llu sinceTick=%llu pc=%04x task=%u\n",
                            (unsigned long long)(cpu.cycles-origin),(unsigned long long)(cpu.cycles-lastTick),cpu.pc,word(0xfdca));
                }
                emu.Step();
                if(tracePitch && pcm.ram2[23][0]!=pitch) {
                    std::printf("PCM_PITCH at=%llu sinceTick=%llu old=%04x new=%04x\n",
                        (unsigned long long)(cpu.cycles-origin),(unsigned long long)(cpu.cycles-lastTick),pitch,pcm.ram2[23][0]);
                }
                pitch=pcm.ram2[23][0];
                const auto now=pcm.voice_mask&pcm.voice_mask_pending;
                if(now!=mask) {
                    std::printf("PCM_KEY at=%llu sinceTick=%llu old=%06x new=%06x\n",
                        (unsigned long long)(cpu.cycles-origin),(unsigned long long)(cpu.cycles-lastTick),mask,now);
                    mask=now;
                }
            }
        };
        advance(400000);
        for(unsigned i=0;i<4;++i) {
            const uint8_t note[]{0x90,uint8_t(60+i),100};
            std::printf("MIDI_NOTE at=%llu sinceTick=%llu\n",(unsigned long long)(cpu.cycles-origin),
                (unsigned long long)(cpu.cycles-lastTick));
            emu.PostMIDI(note); advance(210000+i*20000);
        }
        advance(400000);
        tracePitch=true;
        for(unsigned i=0;i<3;++i) {
            const uint8_t bend[]{0xe0,0,uint8_t(i==1?64:96)};
            std::printf("MIDI_BEND at=%llu sinceTick=%llu\n",(unsigned long long)(cpu.cycles-origin),
                (unsigned long long)(cpu.cycles-lastTick));
            emu.PostMIDI(bend); advance(310000+i*30000);
        }
        return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--mono-pitch") == 0) { TraceMonoPitch(emu); return 0; }
    if(argc == 3 && std::strcmp(argv[2],"--native-nrpn") == 0) { VerifyNrpn(emu); return 0; }
    if(argc == 3 && std::strcmp(argv[2],"--native-rpn") == 0) { VerifyRpn(emu); return 0; }
    if(argc == 3 && std::strcmp(argv[2],"--native-effects-settings") == 0) {
        VerifyEffectsSettings(emu); return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--native-effects-requests") == 0) {
        VerifyEffectsRequests(emu); return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--native-system-defaults") == 0) {
        VerifySystemDefaults(emu); return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--native-melodic-presets") == 0) {
        VerifyMelodicPresets(emu); return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--native-rhythm-presets") == 0) {
        VerifyRhythmPresets(emu); return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--native-all-notes") == 0) {
        VerifyAllNotesOff(emu); return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--native-parts") == 0) {
        VerifyNativeParts(emu); return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--native-master") == 0) {
        VerifyNativeMaster(emu); return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--effects") == 0) {
        TraceEffects(emu); return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--extended") == 0) {
        TraceExtendedSystem(emu); return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--extended-tx") == 0) {
        TraceExtendedSystem(emu,true); return 0;
    }
    if(argc == 3 && std::strcmp(argv[2],"--system") == 0) {
        auto run=[&](unsigned duration,const char* label) {
            const auto end=cpu.cycles+duration;
            std::map<unsigned,unsigned> hits;
            while(cpu.cycles<end) {
                const unsigned pc=(unsigned(cpu.cp)<<16)|cpu.pc;
                if(std::strcmp(label,"gs-reset")==0 || std::strcmp(label,"gm-reset")==0) {
                    switch(pc) {
                        case 0x041247: case 0x041dba: case 0x043798:
                        case 0x04124f: case 0x041284: case 0x040746:
                        case 0x04056e: case 0x0412cf: case 0x0412b8:
                        case 0x0412be: case 0x04379f:
                            std::printf("ORDER %s cycles=%llu task=%u pc=%06x queueWrite=%u queueRead=%u\n",
                                label,(unsigned long long)cpu.cycles,word(0xfdca),pc,
                                MCU_Read(cpu,0xaaf8),MCU_Read(cpu,0xaaf9));
                    }
                }
                switch(pc) {
                    case 0x24b2: case 0x2554: case 0x2563: case 0x040e8e:
                    case 0x040f46: case 0x04101d: case 0x0415da: case 0x041680:
                    case 0x041d88: case 0x0406b8: case 0x0408b8: case 0x041e5d:
                    case 0x604a: case 0x6412: ++hits[pc];
                }
                emu.Step();
            }
            for(auto [pc,count]:hits) std::printf("SYSTEM %s pc=%06x count=%u\n",label,pc,count);
        };
        auto snapshot=[&] {
            std::array<uint8_t,0x1740> values{};
            for(unsigned i=0;i<values.size();++i) values[i]=MCU_Read(cpu,0x8000+i);
            return values;
        };
        auto diff=[&](const auto& before,const auto& after,const char* label) {
            for(unsigned i=0;i<before.size();++i) if(before[i]!=after[i])
                std::printf("DIFF %s ram=%04x old=%02x new=%02x\n",label,0x8000+i,before[i],after[i]);
        };
        auto gs=[&](unsigned a,unsigned b,unsigned c,unsigned value,bool valid=true) {
            const uint8_t msg[]{0xf0,0x41,0x10,0x42,0x12,uint8_t(a),uint8_t(b),uint8_t(c),
                uint8_t(value),uint8_t((128-((a+b+c+value)&127)+(valid?0:1))&127),0xf7};
            emu.PostMIDI(msg);
        };
        run(120000000,"boot");
        for(unsigned base : {0xd6e8u,0xd726u,0xd7dcu,0xd982u,0xdc16u}) {
            for(unsigned n=0;n<128;++n) {
                const unsigned address=0x30000+base+10*n;
                if(n && MCU_Read(cpu,address)==0) break;
                const unsigned kind=MCU_Read(cpu,address+1);
                std::printf("PARAM table=%04x key=%02x kind=%u destination=%04x arg=%04x lower=%04x upper=%04x handler=%04x\n",
                    base,MCU_Read(cpu,address),kind,word(address+2),word(address+4),word(address+6),word(address+8),
                    kind<7?word(0x040f38+2*kind):0xffff);
            }
        }
        const auto boot=snapshot();
        const uint8_t gm[]{0xf0,0x7e,0x7f,0x09,0x01,0xf7};
        for(unsigned mode=0;mode<2;++mode) {
            gs(0x40,0,0x7f,0); run(10000000,"baseline-gs");
            const auto baseline=snapshot();
            for(auto entry : {std::array<unsigned,4>{0x40,0,4,37},
                    {0x40,0x11,0x19,45},{0x40,0x11,0x1c,23},
                    {0x40,0x11,0x30,71},{0x40,1,0x33,17}}) {
                const auto before=snapshot();
                gs(entry[0],entry[1],entry[2],entry[3]);
                char label[40]; std::snprintf(label,sizeof(label),"set-%02x%02x%02x",entry[0],entry[1],entry[2]);
                run(1000000,label); diff(before,snapshot(),label);
            }
            if(mode==0) gs(0x40,0,0x7f,0); else emu.PostMIDI(gm);
            run(40000000,mode==0?"gs-reset":"gm-reset");
            diff(baseline,snapshot(),mode==0?"gs-vs-baseline":"gm-vs-baseline");
            diff(boot,snapshot(),mode==0?"gs-vs-boot":"gm-vs-boot");
        }
        gs(0x40,0,0x7f,0); run(10000000,"baseline-gs");
        const auto before=snapshot(); gs(0x40,0x11,0x19,13,false);
        run(1000000,"bad-checksum"); diff(before,snapshot(),"bad-checksum");
        auto block=[&](unsigned offset,std::initializer_list<uint8_t> data,const char* label) {
            std::vector<uint8_t> msg{0xf0,0x41,0x10,0x42,0x12,0x40,0x11,uint8_t(offset)};
            unsigned sum=0x40+0x11+offset;
            for(auto byte:data) {msg.push_back(byte);sum+=byte;}
            msg.push_back(uint8_t((128-(sum&127))&127)); msg.push_back(0xf7);
            const auto old=snapshot(); emu.PostMIDI(msg); run(1000000,label); diff(old,snapshot(),label);
        };
        block(0x40,{65,66,67,68,69,70,71,72,73,74,75,76},"scale-12-byte");
        block(0x17,{8,1},"fine-tune-2-nibbles");
        block(0x32,{127},"cutoff-clamp");
        return 0;
    }
    if(argc == 3 && (std::strcmp(argv[2],"--events") == 0
        || std::strcmp(argv[2],"--kernel-events") == 0
        || std::strcmp(argv[2],"--configuration-readers") == 0)) {
        KernelEventProbe kernelEvents;
        ConfigurationReaders configurationReaders;
        const bool inspectConfiguration=std::strcmp(argv[2],"--configuration-readers")==0;
#if !defined(SC55_ORACLE_CONFIG_READS)
        if(inspectConfiguration) throw std::runtime_error("Configure with -DSC55_ORACLE_CONFIG_READS=ON to observe configuration reads");
#endif
        if(std::strcmp(argv[2],"--kernel-events")==0) activeKernelEvents=&kernelEvents;
        auto run=[&](unsigned duration,const char* label) {
            const auto end=cpu.cycles+duration;
            std::map<unsigned,unsigned> calls;
            if(inspectConfiguration) activeConfigurationReaders=&configurationReaders;
            while(cpu.cycles<end) {
                const bool dispatch=cpu.cp==0 && cpu.pc==0x84e;
                const auto target=cpu.r[6];
                emu.Step();
                if(dispatch && cpu.cp==0 && cpu.pc==target) ++calls[target];
            }
            activeConfigurationReaders=nullptr;
            for(auto [target,count]:calls)
                std::printf("EVENT %s target=%04x count=%u\n",label,target,count);
        };
        run(120000000,"boot");
        for(unsigned index=0;index<16;++index)
            std::printf("TABLE offset=%02x target=%04x\n",index*2,word(0x852+index*2));
        const uint8_t note[]{0x90,60,100}, off[]{0x80,60,0};
        emu.PostMIDI(note); run(4000000,"note-on");
        emu.PostMIDI(off); run(4000000,"note-off");
        const uint8_t program[]{0xc0,48};
        emu.PostMIDI(program); run(4000000,"program48");
        for(unsigned cc=0;cc<128;++cc) {
            for(unsigned value : {127u,0u}) {
                const uint8_t msg[]{0xb0,uint8_t(cc),uint8_t(value)};
                emu.PostMIDI(msg);
                char label[40]; std::snprintf(label,sizeof(label),"cc%u-value%u",cc,value);
                run(1000000,label);
            }
        }
        const uint8_t gsReset[]{0xf0,0x41,0x10,0x42,0x12,0x40,0x00,0x7f,0x00,0x41,0xf7};
        emu.PostMIDI(gsReset); run(40000000,"gs-reset");
        if(activeKernelEvents) {
            kernelEvents.report(cpu);
            activeKernelEvents=nullptr;
        }
        if(inspectConfiguration) configurationReaders.report();
        return 0;
    }
    for(unsigned phase=0;phase<4;++phase) {
        if(phase==2) { const uint8_t msg[]{0xc0,48,0x90,60,100}; emu.PostMIDI(msg); }
        if(phase==3) { const uint8_t msg[]{0x80,60,0}; emu.PostMIDI(msg); }
        const auto end=cpu.cycles+(phase==0 ? 120000000 : 40000000);
        std::map<std::pair<unsigned,unsigned>,unsigned long long> resumes;
        std::map<unsigned,unsigned long long> markers;
        std::array<unsigned long long,10> elapsed{};
        unsigned long long sleep=0,frames=0;
        emu.SetSampleCallback([](void* p,const AudioFrame<int32_t>&){++*static_cast<unsigned long long*>(p);},&frames);
        while(cpu.cycles<end) {
            const bool resume=cpu.cp==0 && cpu.pc==0x540 && cpu.ex_ignore;
            const unsigned task=word(0xfdca), pc=(unsigned(cpu.cp)<<16)|cpu.pc;
            if(task<10) ++elapsed[task];
            sleep+=cpu.sleep!=0;
            switch(pc) {case 0x599: case 0x5af: case 0x1efb: case 0x5af1: case 0x5af9:
                case 0x5946: case 0x043794: case 0x0444aa: case 0x044540: case 0x037b: case 0x752: ++markers[pc];}
            emu.Step();
            if(resume) ++resumes[{task,(unsigned(cpu.cp)<<16)|cpu.pc}];
        }
        std::printf("PHASE %u cycles=%llu frames=%llu sleepSteps=%llu\n",phase,(unsigned long long)cpu.cycles,frames,sleep);
        // Task 9 is the idle/kernel sentinel, not a task-table entry.
        std::printf("IDLE_KERNEL steps=%llu\n",elapsed[9]);
        for(unsigned task=0;task<9;++task)
            std::printf("TASK %u steps=%llu state=%02x events=%02x mask=%02x period=%u elapsed=%u savedSP=%04x\n",task,elapsed[task],MCU_Read(cpu,0xfdd9+task),MCU_Read(cpu,0xfde2+task),MCU_Read(cpu,0xfdeb+task),word(0xfe24+2*task),MCU_Read(cpu,0xfe08+task),word(0xfdf4+2*task));
        for(auto [key,count]:resumes) std::printf("RESUME task=%u pc=%06x count=%llu\n",key.first,key.second,count);
        for(auto [pc,count]:markers) std::printf("MARK pc=%06x count=%llu\n",pc,count);
    }
}
