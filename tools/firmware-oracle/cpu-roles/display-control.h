#pragma once
#include "sc55_display.h"

// Consumer-side rendering, deliberately outside NativeSynth::render/state.
inline sc55::DisplayFrame::Line RenderDisplayText(const sc55::SynthState& state,const sc55::DisplayView& view)
{
    sc55::DisplayFrame::Line normal; normal.fill(' ');
    if(state.allSelected) std::copy_n("ALL PARTS",9,normal.begin()+4);
    else {
        const auto p=state.selectedPart;
        const auto& part=state.parts[p==9 ? 0 : p<9 ? p+1 : p];
        const unsigned program=unsigned(part.program)+1;
        normal[0]=program>=100 ? uint8_t('0'+program/100) : uint8_t(' ');
        normal[1]=program>=10 ? uint8_t('0'+program/10%10) : uint8_t(' ');
        normal[2]=uint8_t('0'+program%10);
        std::copy(part.name.begin(),part.name.end(),normal.begin()+4);
    }
    return view.text.compose(normal);
}

inline void VerifyDisplayControl(Emulator& emu,const RomsetInfo& roms)
{
    // Consumer cadence must not become the device clock. Compare frequent
    // polling, skipped paints and a fresh consumer reading the same history.
    sc55::DisplayEventLog eventLog;
    sc55::DisplayData eventData;
    sc55::DisplayPresentation frequent,delayed;
    const std::array<uint8_t,20> longText{0x10,0,0,'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q'};
    eventData.write(longText); eventLog.receive(eventData,false,625);
    sc55::DisplayFrame::Line normal; normal.fill(' ');
    {
        sc55::DisplayEventLog plain,fast;
        fast.fastScroll(true,0);
        plain.receive(eventData,false,625); fast.receive(eventData,false,625);
        sc55::DisplayPresentation uiOwned,referenceOwned,ordinary;
        bool changed=false;
        for(uint64_t now=100000;now<=20000000;now+=100000) {
            const auto ui=uiOwned.update(plain.snapshot(now),true);
            const auto reference=referenceOwned.update(fast.snapshot(now));
            const auto slow=ordinary.update(plain.snapshot(now),false);
            if(ui.text.compose(normal)!=reference.text.compose(normal))
                throw std::runtime_error("UI scroll preference differs from reference display control");
            changed|=ui.text.compose(normal)!=slow.text.compose(normal);
        }
        if(!changed) throw std::runtime_error("UI scroll preference had no effect");
    }
    for(uint64_t now=100000;now<=100000000;now+=100000) {
        if(now==9000000) {
            // Replacement must keep the device's shared scroll countdown.
            eventData.write(longText); eventLog.receive(eventData,false,now);
        }
        const auto snapshot=eventLog.snapshot(now);
        const auto a=frequent.update(snapshot);
        if(now%1300000==0) {
            const auto b=delayed.update(snapshot);
            sc55::DisplayPresentation reopened;
            const auto c=reopened.update(snapshot);
            if(a.text.compose(normal)!=b.text.compose(normal) || a.text.visible!=b.text.visible
                || a.text.compose(normal)!=c.text.compose(normal) || a.text.visible!=c.text.visible)
                throw std::runtime_error("UI polling/reopening changed display time");
        }
    }
    for(unsigned i=0;i<sc55::DisplayEvents::capacity+10;++i)
        eventLog.cancel(true,100000000+i);
    const std::array<uint8_t,4> latestText{0x10,0,0,'Z'};
    eventData.write(latestText); eventLog.receive(eventData,false,110000000);
    const auto recovered=delayed.update(eventLog.snapshot(112000000));
    if(!delayed.resyncs() || !recovered.text.visible || recovered.text.compose(normal)[8]!='Z')
        throw std::runtime_error("Display history overrun failed to recover latest text");
    const auto oldExpired=delayed.update(eventLog.snapshot(180000000));
    if(oldExpired.text.visible) throw std::runtime_error("History resync restarted display expiry");
    sc55::DisplayEventLog replacement;
    const auto newSynth=delayed.update(replacement.snapshot(180000000));
    if(newSynth.text.visible || newSynth.bitmapVisible)
        throw std::runtime_error("Display state leaked across synth replacement");
    std::puts("Display owner: slow/reopened UI, repeated text, history overrun and synth replacement PASS");
    auto& cpu=emu.GetMCU();
    const auto boot=cpu.cycles+120000000;
    while(cpu.cycles<boot) emu.Step();
    sc55::DisplayData data;
    sc55::DisplayControl display;
    sc55::DisplayControl::Line rendered{};
    unsigned initialChecks=0,scrollChecks=0,ticks=0,bitmapChecks=0;
    bool receiving=false,scrolling=false,bitmapPending=false;
    const auto line=[&](unsigned address) {
        sc55::DisplayControl::Line result;
        for(unsigned i=0;i<16;++i) result[i]=MCU_Read(cpu,address+i);
        return result;
    };
    const auto run=[&](uint64_t cycles) {
        const auto end=cpu.cycles+cycles;
        while(cpu.cycles<end) {
            if(cpu.cp==4 && cpu.pc==0x3a4f) display.cancelText();
            if(cpu.cp==4 && cpu.pc==0x3a6a) display.cancelBitmap();
            if(cpu.cp==0 && cpu.pc==0x7f5e) {display.timerTicks(1);++ticks;}
            if(cpu.cp==0 && cpu.pc==0x7f6f && initialChecks && !receiving) {
                if(display.textTicks()!=MCU_Read16(cpu,0xcf32)
                    || display.bitmapTicks()!=MCU_Read16(cpu,0xcf34))
                    throw std::runtime_error("Display hold countdown differs from H8");
            }
            if(cpu.cp==4 && cpu.pc==0x738f && bitmapPending) {
                bitmapPending=false;
                display.receive(data);
                for(unsigned i=0;i<64;++i)
                    if(display.bitmap()[i]!=MCU_Read(cpu,0xff00+i))
                        throw std::runtime_error("Native display bitmap differs from H8");
                if(display.bitmapTicks()!=MCU_Read16(cpu,0xcf34))
                    throw std::runtime_error("Bitmap hold initialization differs from H8");
                ++bitmapChecks;
            }
            if(cpu.cp==4 && cpu.pc==0x37dd) {display.receive(data);receiving=true;}
            if(cpu.cp==4 && cpu.pc==0x38a0 && receiving) {
                receiving=false;
                if(data.textLength<=16) {
                    display.service();
                    if(display.frame().compose(line(0xcd14))!=line(0xce7c))
                        throw std::runtime_error("Native short display differs from H8");
                }
                ++initialChecks;
            }
            if(cpu.cp==4 && cpu.pc==0x38a7) {
                display.service((MCU_Read16(cpu,0xcdc8)&8)!=0);
                rendered=display.frame().compose(line(0xce6b));
                scrolling=true;
            }
            if(cpu.cp==4 && cpu.pc==0x38ed && scrolling) {
                scrolling=false;
                if(rendered!=line(0xcd14) || display.scrollOffset()!=MCU_Read(cpu,0xceae)
                    || display.scrollTicks()!=MCU_Read(cpu,0xcf30))
                    throw std::runtime_error("Native scrolling display differs from H8");
                ++scrollChecks;
            }
            emu.Step();
        }
    };
    const auto sendPayload=[&](const std::vector<uint8_t>& payload) {
        bitmapPending=payload[1]==1;
        if(data.write(payload)!=sc55::DisplayData::WriteResult::applied)
            throw std::runtime_error("Invalid display fixture");
        std::vector<uint8_t> packet{0xf0,0x41,0x10,0x45,0x12};
        unsigned sum=0;
        for(auto b:payload) {packet.push_back(b);sum+=b;}
        packet.push_back(uint8_t(-sum)&127);packet.push_back(0xf7);
        emu.PostMIDI(packet);
    };
    const auto send=[&](std::vector<uint8_t> text) {
        std::vector<uint8_t> payload{0x10,0,0};
        payload.insert(payload.end(),text.begin(),text.end());
        sendPayload(payload);
    };
    for(unsigned length:{1u,3u,4u,15u,16u}) {
        send(std::vector<uint8_t>(length,'A'+length)); run(4000000);
    }
    send(std::vector<uint8_t>(32,'Z')); run(160000000);
    send({'N','E','W'}); run(4000000);
    send(std::vector<uint8_t>(17,'X')); run(340000000);
    std::vector<uint8_t> bitmap{0x10,1,0};
    for(unsigned i=0;i<64;++i) bitmap.push_back(uint8_t(i));
    sendPayload(bitmap); run(20000000);
    bitmap[3]=127; sendPayload(bitmap); run(64000000);
    if(display.bitmapActive() || display.textActive())
        throw std::runtime_error("Display messages did not expire");
    if(initialChecks!=8 || scrollChecks<40 || bitmapChecks!=2 || !ticks)
        throw std::runtime_error("Display control fixture did not exercise all paths");
    std::printf("Display control: %u texts, %u scroll services, %u bitmaps, %u timer ticks match H8\n",
        initialChecks,scrollChecks,bitmapChecks,ticks);

    const auto& rom=roms.rom_data;
    const auto& r1=rom[size_t(RomLocation::ROM1)];
    const auto& r2=rom[size_t(RomLocation::ROM2)];
    sc55::NativeSynth synth(sc55::ImportSoundData(r1,r2),r1,r2,
        rom[size_t(RomLocation::WAVEROM1)],rom[size_t(RomLocation::WAVEROM2)],rom[size_t(RomLocation::WAVEROM3)]);
    sc55::DisplayPresentation nativeDisplay;
    const auto readDisplay=[&] {return nativeDisplay.update(synth.state().displayEvents);};
    const auto nativeSend=[&](std::vector<uint8_t> payload,uint8_t model=0x45) {
        std::vector<uint8_t> packet{0xf0,0x41,0x10,model,0x12};
        unsigned sum=0;
        for(auto b:payload) {packet.push_back(b);sum+=b;}
        packet.push_back(uint8_t(-sum)&127);packet.push_back(0xf7);
        if(synth.push(packet)!=packet.size()) throw std::runtime_error("Display MIDI not accepted");
    };
    const auto render=[&](unsigned count) {
        std::array<AudioFrame<int32_t>,257> frames{};
        while(count) {const auto n=std::min(count,257u);synth.render(std::span(frames).first(n));count-=n;}
        if(synth.failed()) throw std::runtime_error("Display render failed");
    };
    nativeSend({0x10,0,0,'T','E','S','T'});render(1600);
    const auto textState=synth.state();
    const auto textView=readDisplay();
    const std::string expected="      TEST      ";
    if(!textView.text.visible || !std::equal(expected.begin(),expected.end(),RenderDisplayText(textState,textView).begin()))
        throw std::runtime_error("Display text did not reach native snapshot");
    for(unsigned i=0;i<100;++i)
        if(RenderDisplayText(synth.state(),readDisplay())!=RenderDisplayText(textState,textView))
            throw std::runtime_error("Snapshot read advanced display");
    nativeSend(bitmap);render(1600);
    const auto bitmapView=readDisplay();
    if(!bitmapView.bitmapVisible
        || !std::equal(bitmap.begin()+3,bitmap.end(),bitmapView.bitmap.begin()))
        throw std::runtime_error("Display bitmap did not reach native snapshot");
    render(100000);
    const auto expired=readDisplay();
    if(expired.text.visible || expired.bitmapVisible)
        throw std::runtime_error("Native display did not expire without an editor");
    std::puts("Native display: MIDI/render/snapshot text and bitmap delivery, editor-independent expiration PASS");
    constexpr std::array<unsigned,18> buttons{
        MCU_BUTTON_PART_L,MCU_BUTTON_PART_R,MCU_BUTTON_INST_L,MCU_BUTTON_INST_R,
        MCU_BUTTON_LEVEL_L,MCU_BUTTON_LEVEL_R,MCU_BUTTON_PAN_L,MCU_BUTTON_PAN_R,
        MCU_BUTTON_REVERB_L,MCU_BUTTON_REVERB_R,MCU_BUTTON_CHORUS_L,MCU_BUTTON_CHORUS_R,
        MCU_BUTTON_KEY_SHIFT_L,MCU_BUTTON_KEY_SHIFT_R,MCU_BUTTON_MIDI_CH_L,MCU_BUTTON_MIDI_CH_R,
        MCU_BUTTON_INST_MUTE,MCU_BUTTON_INST_ALL};
    for(unsigned index=0;index<buttons.size();++index) {
        send({'T','E','S','T'}); nativeSend({0x10,0,0,'T','E','S','T'});
        run(2000000); render(3200);
        sendPayload(bitmap);nativeSend(bitmap);run(2000000);render(3200);
        cpu.button_pressed.store(1u<<buttons[index]);
        if(index<2) synth.selectPart(index ? 1 : -1);
        else if(index<16) {
            constexpr std::array<sc55::PartParameter,7> parameters{
                sc55::PartParameter::program,sc55::PartParameter::volume,sc55::PartParameter::pan,
                sc55::PartParameter::reverb,sc55::PartParameter::chorus,sc55::PartParameter::keyShift,
                sc55::PartParameter::channel};
            synth.adjustSelectedPart(parameters[(index-2)/2],index&1 ? 1 : -1);
        } else if(index==16) synth.toggleMute();
        else synth.toggleAll();
        run(2000000);render(3200);
        cpu.button_pressed.store(0);run(2000000);render(3200);
        const auto state=readDisplay();
        if(state.text.visible!=(MCU_Read16(cpu,0xcf32)!=0)
            || state.bitmapVisible!=(MCU_Read16(cpu,0xcf34)!=0)) {
            std::printf("Display cancel mismatch button=%u native=%u/%u H8=%u/%u\n",buttons[index],
                state.text.visible,state.bitmapVisible,MCU_Read16(cpu,0xcf32),MCU_Read16(cpu,0xcf34));
            throw std::runtime_error("Native panel display cancellation differs from H8");
        }
    }
    std::puts("Native display: all 18 normal front-panel buttons match H8 text/bitmap cancellation");
    nativeSend({0x10,0,0,'R','E','S','E','T'});nativeSend(bitmap);render(3200);
    nativeSend({0x40,0,0x7f,0},0x42);render(32000);
    const auto resetView=readDisplay();
    if(resetView.text.visible || resetView.bitmapVisible)
        throw std::runtime_error("Native GS reset left a display message active");

    sc55::NativeSynth split(sc55::ImportSoundData(r1,r2),r1,r2,
        rom[size_t(RomLocation::WAVEROM1)],rom[size_t(RomLocation::WAVEROM2)],rom[size_t(RomLocation::WAVEROM3)]);
    sc55::NativeSynth whole(sc55::ImportSoundData(r1,r2),r1,r2,
        rom[size_t(RomLocation::WAVEROM1)],rom[size_t(RomLocation::WAVEROM2)],rom[size_t(RomLocation::WAVEROM3)]);
    sc55::NativeSynth noDisplay(sc55::ImportSoundData(r1,r2),r1,r2,
        rom[size_t(RomLocation::WAVEROM1)],rom[size_t(RomLocation::WAVEROM2)],rom[size_t(RomLocation::WAVEROM3)]);
    std::vector<uint8_t> packet{0xf0,0x41,0x10,0x45,0x12,0x10,0,0};
    unsigned sum=0x10;
    for(unsigned i=0;i<32;++i) {packet.push_back(uint8_t('A'+i%26));sum+=packet.back();}
    packet.push_back(uint8_t(-sum)&127);packet.push_back(0xf7);
    split.push(packet);whole.push(packet);
    for(unsigned key=40;key<64;++key) {
        const uint8_t note[]{0x90,uint8_t(key),100};
        split.push(note);whole.push(note);noDisplay.push(note);
    }
    std::array<AudioFrame<int32_t>,257> a{},b{},withoutDisplay{};
    uint64_t nonzeroFrames=0;
    sc55::DisplayPresentation wholeDisplay,splitDisplay;
    for(unsigned block=0;block<160;++block) {
        whole.render(a);
        noDisplay.render(withoutDisplay);
        split.render({});
        for(auto& frame:b) split.render(std::span(&frame,1));
        const auto x=whole.state(),y=split.state();
        const auto xv=wholeDisplay.update(x.displayEvents),yv=splitDisplay.update(y.displayEvents);
        if(whole.failed() || split.failed() || RenderDisplayText(x,xv)!=RenderDisplayText(y,yv)
            || xv.text.visible!=yv.text.visible)
            throw std::runtime_error("Block partition changed scrolling display");
        for(unsigned i=0;i<a.size();++i) {
            if(a[i].left!=b[i].left || a[i].right!=b[i].right)
                throw std::runtime_error("Display scheduling changed audio partition behavior");
            if(a[i].left!=withoutDisplay[i].left || a[i].right!=withoutDisplay[i].right)
                throw std::runtime_error("Display message changed sounding audio");
            nonzeroFrames+=a[i].left!=0 || a[i].right!=0;
        }
    }
    if(!nonzeroFrames || noDisplay.failed()) throw std::runtime_error("Display audio independence fixture is silent/failed");
    std::printf("Display/audio independence: 24 notes, %llu nonzero frames match with/without display messages\n",
        (unsigned long long)nonzeroFrames);
    std::puts("Native display: GS reset cancellation and zero/1/257-frame scrolling partitions PASS");
}
