#pragma once
#include "sc55_native_player.h"
#include "sc55_sound_data_import.h"

inline int verifyNativePlayer(const char* assetPath,const char* waveDirectory)
{
    const auto require = [](bool value,const char* message) {
        if (!value) throw std::runtime_error(message);
    };
    std::ifstream input(assetPath,std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    sc55::SoundData data;
    require(data.loadEncoded(bytes),"Cannot load native test sound data");
    auto mcu = std::make_unique<mcu_t>();
    mcu->is_mk1 = true;
    auto pcm = std::make_unique<pcm_t>();
    PCM_Init(*pcm,*mcu); PCM_UseSimulation(*pcm,false);
    for (unsigned bank = 0; bank < 3; ++bank)
    {
        std::ifstream wave(std::filesystem::path(waveDirectory) / ("sc55_waverom"+std::to_string(bank+1)+".bin"),std::ios::binary);
        const std::vector<uint8_t> raw{std::istreambuf_iterator<char>(wave),{}};
        require(raw.size() == 0x100000,"Invalid waveform input");
        auto* target = bank == 0 ? pcm->waverom1 : bank == 1 ? pcm->waverom2 : pcm->waverom3;
        unscramble(raw.data(),target,int(raw.size()));
    }
    struct Output { int32_t low = INT32_MAX, high = INT32_MIN; uint64_t frames = 0; } output;
    mcu->callback_userdata = &output;
    mcu->sample_callback = [](void* opaque,const AudioFrame<int32_t>& frame) {
        auto& out = *static_cast<Output*>(opaque);
        ++out.frames; out.low = std::min({out.low,frame.left,frame.right});
        out.high = std::max({out.high,frame.left,frame.right});
    };
    auto player = std::make_unique<sc55::NativeMelodicPlayer>(data,*pcm);
    require(PCM_GetOutputFrequency(*pcm)==32000,"Native PCM sample-rate contract differs");
    require(pcm->ram2[30][10]==0xffff,"Native PCM random source was not seeded");
    // Cross several non-integral control deadlines (160256/625 passes).
    // Requested durations lose the last pass's overshoot on every period.
    sc55::ControlTaskClock expectedClock;
    for(unsigned step=0;step<1024;++step) {
        const auto before=pcm->cycles;
        player->step(); expectedClock.advance(pcm->cycles-before);
        require(!player->failed() && player->controlCyclesUntilNextTick()==expectedClock.untilNextExpiration(),
            "Native control clock drifted from actual PCM time");
    }
    require(pcm->ram2[30][10]!=0 && pcm->ram2[30][10]!=0xffff,
        "Native PCM random source did not advance");
    const auto send = [&](std::initializer_list<uint8_t> message) {
        // Deliberately fragment every MIDI message across individual pushes.
        for (auto byte : message) require(player->push(std::span(&byte,1)) == 1,"Native ingress rejected byte");
    };
    const auto run = [&](unsigned frames) {
        const auto until = output.frames + frames;
        while (output.frames < until)
        {
            player->step();
            require(!player->failed(),"Native scheduler failed");
        }
    };
    run(33);
    {
        const auto unsupported=player->unsupportedEvents();
        send({0xf0,0x41,0x10,0x45,0x12,0x10,0,0,'T','E','S','T',0x30,0xf7});
        run(2);
        require(player->displayData().textRevision==1 && player->displayData().textLength==4
            && player->displayData().text[0]=='T' && player->unsupportedEvents()==unsupported,
            "Model45 text did not reach native display owner");
        const auto resets=player->completedResets();
        send({0xf0,0x7e,0x7f,9,2,0xf7}); run(2);
        require(player->unsupportedEvents()==unsupported && !player->resetPending()
            && player->completedResets()==resets,"GM Off incorrectly reset native state");
    }
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,4,37,0x17,0xf7});
    run(2);
    require(player->masterControls().volume == 37,"Native GS master volume not applied");
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,4,99,0,0xf7});
    run(2);
    require(player->masterControls().volume == 37 && player->rejectedSysEx() == 1,
        "Native invalid SysEx changed master state");
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,4,100,0x58,0xf7});
    run(2);
    require(player->masterControls().volume == 100,"Native master volume restore failed");
    send({0xc0,48}); send({0x90,60,100});
    run(8000);
    require(int64_t(output.high)-output.low > 65536,"Master-control test note silent");
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,4,0,0x3c,0xf7});
    run(16000); // settle the existing PCM envelope ramp
    output.low = INT32_MAX; output.high = INT32_MIN;
    run(1024);
    require(int64_t(output.high)-output.low < 65536,"Master mute did not reach active PCM voice");
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,4,100,0x58,0xf7});
    output.low = INT32_MAX; output.high = INT32_MIN;
    run(8000);
    require(int64_t(output.high)-output.low > 65536,"Master unmute did not restore active PCM voice");
    send({0xb0,120,0}); run(128000);
    require(player->freeVoices() == 24,"Master-control test voice not reclaimed");
    output.low = INT32_MAX; output.high = INT32_MIN;
    for (uint8_t program : {uint8_t(0),uint8_t(48),uint8_t(80)})
    {
        send({0xc0,program});
        send({0x90,60,100});
        run(8000);
        require(int64_t(output.high)-output.low > 65536,"Native MIDI note produced no audio");
        send({0xb0,64,127}); send({0x80,60,0});
        run(2000);
        require(player->freeVoices() < 24,"Sustain did not retain note");
        send({0xe0,0,96}); send({0xb0,7,80}); send({0xb0,64,0});
        run(128000);
        require(player->freeVoices() == 24,"Released voices not reclaimed");
        send({0xe0,0,64});
        output.low = INT32_MAX; output.high = INT32_MIN;
    }
    send({0xc0,48});
    for (uint8_t key = 40; key < 70; ++key) send({0x90,key,100});
    run(32000);
    require(player->queuedEvents() == 0,"Capacity recovery left MIDI stuck");
    require(player->freeVoices() == 0,"Chord did not occupy all 24 slots");
    send({0xb0,120,0});
    run(128000);
    require(player->freeVoices() == 24,"Panic did not reclaim all slots");
    send({0xb0,120,0});
    run(1);
    require(player->freeVoices() == 24,"Repeated panic changed free-voice bookkeeping");
    require(player->unsupportedEvents() == 0,"Supported test MIDI was rejected");
    send({0x99,36,100});
    run(1);
    require(player->unsupportedEvents() == 1,"Unsupported drum was silently substituted");
    // Setup-time routing: one channel addresses two independent GS-order parts.
    // The preceding player is fully stopped before handing this PCM to another.
    sc55::PartSettings settings;
    settings.routing[2].channel = 0;
    player = std::make_unique<sc55::NativeMelodicPlayer>(data,*pcm,settings);
    send({0xc0,48}); send({0x90,60,100}); run(8000);
    require(player->queuedEvents() == 0 && player->partVoiceCount(1) > 0
        && player->partVoiceCount(1) == player->partVoiceCount(2)
        && player->partVoiceCount(0) == 0,"Native NoteOn fanout lost or duplicated a part");
    send({0xb0,7,55}); run(2);
    require(player->partSettings().parts[1].controls.volume == 55
        && player->partSettings().parts[2].controls.volume == 55,"Routed CC did not fan out");
    send({0x80,60,0}); run(128000);
    require(player->freeVoices() == 24,"Routed release did not reclaim both parts");
    // GS part1 receive-note gate: part2 remains on the same input channel.
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,8,0,0x27,0xf7});
    send({0x90,62,100}); run(8000);
    require(player->partVoiceCount(1) == 0 && player->partVoiceCount(2) > 0,
        "GS receive-note setting did not gate native allocation");
    send({0x80,62,0}); run(128000);
    require(player->freeVoices() == 24,"Enabled part release stalled");
    // Invalid/inverted key range must reject new notes, not normalize bounds.
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0x12,0x1d,80,40,0x19,0xf7});
    send({0x90,60,100}); run(8000);
    require(player->freeVoices() == 24,"GS key range did not reach note admission");
    require(player->unsupportedEvents() == 0 && player->rejectedSysEx() == 0,
        "Native routing/settings test rejected supported input");
    // Restore both parts, then verify CC123 keeps sustain rather than hard stopping.
    player = std::make_unique<sc55::NativeMelodicPlayer>(data,*pcm,settings);
    send({0xc0,48}); send({0x90,60,100}); send({0xb0,64,127}); run(8000);
    send({0xb0,123,0}); run(32000);
    require(player->partVoiceCount(1)>0 && player->partVoiceCount(2)>0,
        "All Notes Off incorrectly ignored sustain");
    send({0xb0,121,0}); run(128000);
    require(player->freeVoices()==24,"Reset Controllers failed to release held notes");
    send({0xb0,7,73}); send({0xb0,11,17}); send({0xb0,67,127});
    send({0x90,64,100}); send({0xb0,66,127}); send({0x80,64,0}); run(8000);
    // Change GS part1 from MIDI channel1 to3, releasing its captured note only.
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,2,2,0x2b,0xf7}); run(128000);
    require(player->partSettings().routing[1].channel==2 && player->partVoiceCount(1)==0
        && player->partVoiceCount(2)>0,"Channel change did not isolate the old part's release");
    const auto& reset = player->partSettings().parts[1].controls;
    require(reset.expression==127 && !reset.softPedal && reset.volume==73 && reset.program==48,
        "Channel change reset the wrong controls");
    send({0xb0,121,0}); run(128000);
    send({0x92,65,100}); run(8000);
    require(player->partVoiceCount(1)>0 && player->partVoiceCount(2)==0,
        "New receive channel did not reach native note admission");
    // Assigning the same channel still resets and releases, per firmware arg6.
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,2,2,0x2b,0xf7}); run(128000);
    require(player->partVoiceCount(1)==0 && player->partVoiceCount(3)>0
        && player->unsupportedEvents()==0,
        "Repeated receive-channel assignment did not isolate its side effects");
    send({0xb2,120,0}); run(128000);
    require(player->freeVoices()==24,"Channel reset test left active voices");
    const auto readRom = [&](const char* name) {
        std::ifstream rom(std::filesystem::path(waveDirectory)/name,std::ios::binary);
        return std::vector<uint8_t>{std::istreambuf_iterator<char>(rom),{}};
    };
    const auto rhythm = sc55::ImportRhythmPresets(readRom("sc55_rom1.bin"),readRom("sc55_rom2.bin"));
    player = std::make_unique<sc55::NativeMelodicPlayer>(data,*pcm,sc55::PartSettings{},rhythm);
    for (uint8_t program : {0,8,16,24,25,32,40,48,56,127})
    {
        const auto resolved = rhythm.resolve(program);
        require(bool(resolved),"Missing expected rhythm preset");
        const auto& record = rhythm.records[rhythm.programs[*resolved]];
        const auto map = sc55::RhythmPresetTable::decode(record);
        uint8_t key = 38;
        if (map.tones[key]&0x8000)
            for (key = 0; key < 127 && ((map.tones[key]&0x8000) || !(map.flags[key]&0x10)); ++key) {}
        send({0xc9,program}); send({0x99,key,100});
        output.low = INT32_MAX; output.high = INT32_MIN;
        run(8000);
        require(int64_t(output.high)-output.low > 65536,"Routed rhythm preset produced no PCM audio");
        send({0xb9,120,0}); run(128000);
        require(player->freeVoices()==24,"Routed drum stop failed");
    }
    send({0xc9,0}); send({0x99,46,100}); run(1000);
    require(player->partVoiceCount(0)>0,"Open hi-hat did not allocate a voice");
    send({0x99,42,100}); run(1000); // hi-hat exclusive-group replacement
    require(player->partVoiceCount(0)>0 && player->partVoiceCount(0)<=2,
        "Closed hi-hat did not replace open hi-hat");
    send({0xb9,120,0}); run(128000);
    send({0xc9,64}); send({0x99,38,100}); run(8000);
    require(player->freeVoices()==24,"Invalid drum program reused the old tone");
    require(player->unsupportedEvents()==0,"Routed rhythm used an unsupported fallback");
    sc55::PartSettings drumRouting;
    drumRouting.routing[2].noteFlags = 0xd0; // second map, independent preset
    drumRouting.routing[3].noteFlags = 0xb0; // first map, shared with part0
    player = std::make_unique<sc55::NativeMelodicPlayer>(data,*pcm,drumRouting,rhythm);
    send({0xc9,8}); run(4);
    require(player->partSettings().parts[3].controls.program == 8
        && player->partSettings().parts[2].controls.program == 0,
        "Drum program did not propagate only to the matching map");
    send({0xc1,16}); run(4);
    require(player->partSettings().parts[0].controls.program == 8
        && player->partSettings().parts[2].controls.program == 16,
        "Second drum map changed the first map's program");
    auto mutedRhythm = rhythm;
    mutedRhythm.records[mutedRhythm.programs[0]][0x100+38] = 0;
    player = std::make_unique<sc55::NativeMelodicPlayer>(data,*pcm,sc55::PartSettings{},mutedRhythm);
    send({0x99,38,100});
    output.low = INT32_MAX; output.high = INT32_MIN; run(8000);
    require(int64_t(output.high)-output.low < 65536,"Per-key drum volume was ignored");
    send({0xb9,120,0}); run(128000);
    require(player->freeVoices()==24,"Muted drum voice was not reclaimed");
    const auto drumWrite = [&](uint8_t field,uint8_t key,uint8_t value) {
        send({0xf0,0x41,0x10,0x42,0x12,0x41,field,key,value,
            uint8_t((128-((0x41+field+key+value)&127))&127),0xf7});
    };
    drumWrite(2,38,127); send({0x99,38,100});
    output.low = INT32_MAX; output.high = INT32_MIN; run(8000);
    require(int64_t(output.high)-output.low > 65536,"GS drum level did not restore PCM output");
    send({0xb9,120,0}); run(128000);
    drumWrite(8,38,0); send({0x99,38,100}); run(1000);
    require(player->freeVoices()==24,"GS drum receive disable did not gate Note On");
    drumWrite(8,38,1); send({0x99,38,100}); run(1000);
    require(player->partVoiceCount(0)>0,"GS drum receive enable did not restore Note On");
    send({0xb9,120,0}); run(128000);
    require(player->unsupportedEvents()==0 && player->rejectedSysEx()==0,
        "Supported GS drum controls were rejected");
    const auto melodic = sc55::ImportMelodicPresets(readRom("sc55_rom1.bin"),readRom("sc55_rom2.bin"));
    player = std::make_unique<sc55::NativeMelodicPlayer>(data,*pcm,sc55::PartSettings{},rhythm,melodic);
    for (const auto request : {std::array<uint8_t,2>{8,0},{16,0},{8,4},{8,48},{127,0}})
    {
        const auto before = player->selectedTone(1);
        send({0xb0,0,request[0]}); run(4);
        require(player->selectedTone(1)==before,"Bank select changed tone before Program Change");
        send({0xc0,request[1]}); run(4);
        const auto expected = melodic.resolve(request[0],request[1]);
        require(expected && player->selectedTone(1)==expected->tone,"Variation tone selection mismatch");
        send({0x90,60,100});
        output.low = INT32_MAX; output.high = INT32_MIN; run(8000);
        require(int64_t(output.high)-output.low > 65536,"Native variation produced no PCM output");
        send({0xb0,120,0}); run(128000);
        require(player->freeVoices()==24,"Variation voices not reclaimed");
    }
    send({0xb0,0,64}); send({0xc0,0}); send({0x90,60,100}); run(1000);
    require(!player->selectedTone(1) && player->freeVoices()==24,
        "Invalid melodic selection played a fallback tone");
    require(player->unsupportedEvents()==0,"Imported bank selection used an unsupported path");
    // Disable MIDI Program Change, then address the same part via GS.
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,5,0,0x2a,0xf7}); run(4);
    send({0xc0,48}); run(4);
    require(!player->selectedTone(1),"Disabled MIDI program receiver accepted a change");
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0,8,4,0x23,0xf7}); run(4);
    require(player->selectedTone(1)==melodic.resolve(8,4)->tone,"GS tone pair failed with MIDI program disabled");
    send({0x90,60,100}); output.low=INT32_MAX; output.high=INT32_MIN; run(8000);
    require(int64_t(output.high)-output.low>65536,"GS-selected tone produced no PCM");
    send({0xb0,120,0}); run(128000);
    const auto gsTone = player->selectedTone(1);
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0,0,0x2f,0xf7}); run(4);
    require(player->selectedTone(1)==gsTone && player->rejectedSysEx()==1,
        "Incomplete GS tone pair changed the instrument");
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0x10,0,0,8,0x28,0xf7});
    send({0x99,38,100}); output.low=INT32_MAX; output.high=INT32_MIN; run(8000);
    require(player->partSettings().parts[0].controls.program==8
        && int64_t(output.high)-output.low>65536,"GS drum set selection failed");
    send({0xb9,120,0}); run(128000);
    const auto defaults=sc55::ImportSystemDefaults(readRom("sc55_rom1.bin"),readRom("sc55_rom2.bin"));
    player=std::make_unique<sc55::NativeMelodicPlayer>(data,*pcm,defaults,rhythm,melodic);
    require(player->masterControls().volume==127 && player->masterControls().tune==1024,
        "ROM master defaults were not installed");
    send({0xc0,48}); send({0xb0,1,127}); send({0x90,60,100});
    output.low=INT32_MAX; output.high=INT32_MIN; run(8000);
    require(int64_t(output.high)-output.low>65536,"ROM-backed configuration did not produce PCM");
    send({0xb0,120,0}); run(128000);
    require(player->freeVoices()==24 && player->unsupportedEvents()==0,"ROM-backed note did not finish");
    // Reset must drain live PCM owners without consuming the following MIDI.
    send({0xc0,48}); send({0xb0,64,127}); send({0x90,60,100});
    send({0xc9,8}); send({0x99,38,100}); run(8000);
    send({0x80,60,0});
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,4,37,0x17,0xf7}); run(8);
    require(player->freeVoices()<24,"Reset fixture has no active voices");
    // No reset before EOX, even with a complete address and checksum.
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41}); run(4);
    require(!player->resetPending() && player->completedResets()==0,"Reset ran before EOX");
    send({0xf7}); send({0xc0,80}); send({0x90,67,100});
    player->step();
    require(player->resetPending() && player->queuedEvents()>0,
        "Reset did not retain following MIDI while draining");
    require(player->masterControls().volume==37,"Reset restored config before draining PCM");
    const auto resetFrames=output.frames;
    for (unsigned slice=0;slice<256 && player->resetPending();++slice) run(1024);
    require(!player->resetPending() && player->completedResets()==1,"GS reset did not complete");
    require(output.frames>resetFrames,"PCM time stopped during reset");
    output.low=INT32_MAX; output.high=INT32_MIN; run(8000);
    require(player->masterControls().volume==127 && player->queuedEvents()==0
        && player->partSettings().parts[0].controls.program==0
        && player->partSettings().parts[1].controls.program==80
        && player->selectedTone(1)==melodic.resolve(0,80)->tone
        && int64_t(output.high)-output.low>65536,"Reset defaults or following note incorrect");
    send({0x80,67,0}); run(128000);
    require(player->freeVoices()==24,"Reset retained old sustain state");
    // Consecutive idle GM resets each consume their own EOX exactly once.
    send({0xf0,0x7e,0x7f,9,1,0xf7});
    send({0xf0,0x7e,0x7f,9,1,0xf7}); run(1024);
    require(player->completedResets()==3 && !player->resetPending() && player->queuedEvents()==0
        && player->partSettings().parts[1].controls.program==0
        && player->unsupportedEvents()==0,"Repeated GM reset lost or duplicated input");
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0,0xf7}); run(4);
    require(player->completedResets()==3 && player->rejectedSysEx()==1,
        "Bad checksum triggered a reset");
    send({0xc0,48});
    for (uint8_t key=40;key<64;++key) send({0x90,key,100});
    run(32000);
    require(player->freeVoices()==0,"Reset saturation fixture did not fill PCM slots");
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7});
    for (unsigned slice=0;slice<256 && player->completedResets()!=4;++slice) run(1024);
    require(player->completedResets()==4 && player->freeVoices()==24
        && player->queuedEvents()==0,"Saturated reset left live voices or stuck MIDI");
    const auto effects=sc55::ImportEffectsTables(readRom("sc55_rom1.bin"),readRom("sc55_rom2.bin"));
    player=std::make_unique<sc55::NativeMelodicPlayer>(data,*pcm,defaults,rhythm,melodic,effects);
    run(32000);
    require(player->effectsSettled(),"Native effects boot setup did not settle");
    const auto gsEffect=[&](uint8_t address,uint8_t value) {
        const uint8_t message[]{0xf0,0x41,0x10,0x42,0x12,0x40,1,address,value,
            uint8_t((128-((0x41+address+value)&127))&127),0xf7};
        require(player->push(message)==sizeof(message),"Effects SysEx ingress failed");
    };
    require(pcm->ram2[30][2] == (unsigned(defaults.bytes[0x2d])<<8)
        && (pcm->ram2[31][2]>>8)==defaults.bytes[0x34],"Native effects boot coefficients incorrect");
    for (uint8_t macro=0;macro<8;++macro) {
        gsEffect(0x30,macro); gsEffect(0x38,macro); run(64000);
        require(player->effectsSettled() && player->queuedEvents()==0,
            "GS effects macro transition did not settle");
        require((pcm->ram2[30][2]>>8)==effects.reverbMacros[macro][2]
            && (pcm->ram2[31][2]>>8)==effects.chorusMacros[macro][1],
            "GS effects macro did not reach PCM");
    }
    gsEffect(0x33,17); gsEffect(0x3a,29); run(16000);
    require((pcm->ram2[30][2]>>8)==17 && (pcm->ram2[31][2]>>8)==29,
        "GS scalar effect levels did not reach PCM");
    gsEffect(0x30,4); gsEffect(0x38,2); run(1024);
    gsEffect(0x30,0); gsEffect(0x38,0); run(64000);
    require(player->effectsSettled() && player->unsupportedEvents()==0,
        "Overlapping GS effects changes failed");
    send({0xc0,48}); send({0xb0,91,127}); send({0xb0,93,127}); send({0x90,60,100});
    run(8000); send({0xb0,120,0});
    for (unsigned i=0;i<256 && player->freeVoices()!=24;++i) run(128);
    require(player->freeVoices()==24,"Wet note did not reclaim");
    output.low=INT32_MAX; output.high=INT32_MIN; run(1024);
    require(int64_t(output.high)-output.low>65536,"No effect tail after all voices reclaimed");
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7});
    send({0xc0,80}); send({0x90,67,100}); run(64000);
    require(player->completedResets()==1 && player->effectsSettled() && player->queuedEvents()==0
        && player->partSettings().parts[1].controls.program==80,
        "Effects reset did not resume following MIDI");
    send({0xb0,101,0}); send({0xb0,100,1}); send({0xb0,6,0}); run(2048);
    std::array<uint16_t,24> lowPitch{};
    for (unsigned slot=0;slot<24;++slot) lowPitch[slot]=pcm->ram2[slot][0];
    send({0xb0,6,127}); send({0xb0,38,127}); run(2048);
    unsigned raised=0;
    for (unsigned slot=0;slot<24;++slot) raised+=pcm->ram2[slot][0]>lowPitch[slot];
    require(raised>0 && player->partSettings().parts[1].controls.finePitch()==999,
        "RPN fine tuning did not reach live PCM pitch");
    send({0xb0,100,0}); send({0xb0,6,24}); send({0xe0,127,127}); run(2048);
    require(player->partSettings().parts[1].controls.bendRange==24,
        "RPN bend range not accepted");
    {
        const auto unsupported=player->unsupportedEvents();
        const auto bentPitch=pcm->ram2[23][0];
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0x21,0x10,64,0x4f,0xf7});
        send({0xe0,127,127}); run(2048);
        require(pcm->ram2[23][0]<bentPitch && player->unsupportedEvents()==unsupported,
            "GS bend sensitivity did not reach live PCM pitch");
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0x17,0,8,0x10,0xf7}); run(512);
        const auto low=pcm->ram2[23][0];
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0x17,15,8,1,0xf7}); run(512);
        require(pcm->ram2[23][0]>low && player->partSettings().parts[1].fineTune==248,
            "GS fine tune did not reach live PCM pitch correction");
    }
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7}); run(128000);
    const auto beforeNrpn=player->unsupportedEvents();
    send({0xb9,99,0x1a}); send({0xb9,98,38}); send({0xb9,6,0});
    send({0x99,38,100}); output.low=INT32_MAX; output.high=INT32_MIN; run(8000);
    require(int64_t(output.high)-output.low<65536,"NRPN drum key level zero did not mute");
    send({0xb9,120,0}); run(128000);
    send({0xb9,6,127}); send({0x99,38,100});
    output.low=INT32_MAX; output.high=INT32_MIN; run(8000);
    require(int64_t(output.high)-output.low>65536 && player->unsupportedEvents()==beforeNrpn,
        "NRPN drum key level restore failed");
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7}); run(128000);
    send({0xc0,80}); send({0xb0,99,1}); send({0xb0,98,0x63}); send({0xb0,6,114});
    output.low=INT32_MAX; output.high=INT32_MIN;
    send({0x90,60,100}); run(512);
    const auto slowAttack=int64_t(output.high)-output.low;
    send({0xb0,120,0}); run(128000);
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0x34,14,0x6d,0xf7});
    output.low=INT32_MAX; output.high=INT32_MIN;
    send({0x90,60,100}); run(512);
    require(int64_t(output.high)-output.low>slowAttack,
        "GS envelope attack did not change real PCM onset after NRPN slow attack");
    const auto beforeTone=player->unsupportedEvents();
    for (uint8_t number:{uint8_t(8),uint8_t(9),uint8_t(0x20),uint8_t(0x21),
        uint8_t(0x63),uint8_t(0x64),uint8_t(0x66),uint8_t(0x0a)}) {
        send({0xb0,98,number}); send({0xb0,6,127}); run(512);
    }
    require(player->unsupportedEvents()==beforeTone
        && player->partSettings().parts[1].controls.tone.values[2]==80,
        "Live tone NRPN update failed");
    // Fresh PCM owners make pitch-register comparisons independent of stale
    // slots, prior note allocation and effects tails. No H8 is executed.
    const auto initialPolyPitch=[&](int16_t finePitch,uint8_t gsFine=128) {
        auto testPcm=std::make_unique<pcm_t>();
        PCM_Init(*testPcm,*mcu); PCM_UseSimulation(*testPcm,false);
        std::copy_n(pcm->waverom1,0x100000,testPcm->waverom1);
        std::copy_n(pcm->waverom2,0x100000,testPcm->waverom2);
        std::copy_n(pcm->waverom3,0x100000,testPcm->waverom3);
        auto settings=defaults.parts(true);
        settings.parts[1].controls.program=80;
        settings.parts[1].controls.finePitchValue=finePitch;
        settings.parts[1].fineTune=gsFine;
        auto testPlayer=std::make_unique<sc55::NativeMelodicPlayer>(data,*testPcm,settings,rhythm,melodic);
        const uint8_t note[]{0x90,60,100}; testPlayer->push(note);
        for(unsigned step=0;step<64;++step) {
            testPlayer->step(); require(!testPlayer->failed(),"Initial tuning player failed");
            if(testPcm->ram2[23][0]!=0) return testPcm->ram2[23][0];
        }
        throw std::runtime_error("Initial tuning note did not reach PCM");
    };
    require(initialPolyPitch(900)>initialPolyPitch(0),
        "Ordinary poly onset cancelled fine tuning with a spurious glide source");
    require(initialPolyPitch(0,248)>initialPolyPitch(0,8),
        "GS fine tune did not reach initial PCM pitch correction");
    const auto drumPitch=[&](uint8_t program,uint8_t value,bool repeat) {
        auto testPcm=std::make_unique<pcm_t>();
        PCM_Init(*testPcm,*mcu); PCM_UseSimulation(*testPcm,false);
        std::copy_n(pcm->waverom1,0x100000,testPcm->waverom1);
        std::copy_n(pcm->waverom2,0x100000,testPcm->waverom2);
        std::copy_n(pcm->waverom3,0x100000,testPcm->waverom3);
        auto settings=defaults.parts(true);
        auto testPlayer=std::make_unique<sc55::NativeMelodicPlayer>(data,*testPcm,settings,rhythm,melodic);
        const uint8_t message[]{0xc9,program,0xb9,99,0x18,98,38,6,value};
        require(testPlayer->push(message)==sizeof(message),"Relative pitch MIDI rejected");
        if (repeat) {
            const uint8_t again[]{0xb9,6,value}; testPlayer->push(again);
        }
        const uint8_t note[]{0x99,38,100}; testPlayer->push(note);
        const auto until=output.frames+512;
        while(output.frames<until) {
            testPlayer->step(); require(!testPlayer->failed(),"Relative pitch player failed");
        }
        require(testPlayer->partVoiceCount(0)>0 && testPlayer->unsupportedEvents()==0,
            "Relative pitch note did not start");
        std::array<uint16_t,24> pitches{};
        for(unsigned slot=0;slot<24;++slot) pitches[slot]=testPcm->ram2[slot][0];
        std::sort(pitches.begin(),pitches.end());
        return pitches;
    };
    const auto capitalPitch=drumPitch(0,64,false);
    require(drumPitch(1,64,false)==capitalPitch,"Drum fallback changed baseline pitch");
    const auto raisedPitch=drumPitch(1,76,false);
    require(raisedPitch.back()>capitalPitch.back(),"Relative pitch did not raise PCM pitch");
    require(drumPitch(1,76,true)==raisedPitch,"Repeated relative pitch accumulated");
    std::array<uint16_t,2> sourcePitch{};
    for (unsigned withSource=0;withSource<2;++withSource) {
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7}); run(128000);
        const auto unsupported=player->unsupportedEvents();
        send({0xc0,80}); send({0xb0,126,1}); send({0xb0,5,127}); send({0xb0,65,0});
        if(withSource) {
            send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7e,85,0x6d,0xf7});
            send({0xb0,85,48}); send({0xb0,65,0}); run(64);
            require(player->portamentoSource(1)==48,"CC65 off lost source latch");
            send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7e,84,0x6e,0xf7});
        }
        send({0x90,60,80}); run(512);
        require(player->portamentoSource(1)==255 && player->currentMonoKey(1)==60,
            "Source note did not start/consume latch");
        // Reset initializes the free list at23; this fresh paired note owns
        // slots23/22. Do not include stale PCM registers from inactive slots.
        sourcePitch[withSource]=std::max(pcm->ram2[23][0],pcm->ram2[22][0]);
        const auto voices=player->partVoiceCount(1);
        send({0xb0,84,60}); send({0x90,67,100}); run(2048);
        require(player->partVoiceCount(1)==voices && player->currentMonoKey(1)==67
            && player->portamentoSource(1)==255,"Source legato did not reuse group");
        send({0x80,67,0}); run(2048);
        require(player->currentMonoKey(1)==60,"Source legato release lost held key");
        send({0xb0,84,48}); send({0xb0,65,127}); run(64);
        require(player->portamentoSource(1)==255,"CC65 on did not clear source");
        send({0xb0,84,48}); send({0xb0,121,0}); run(64);
        require(player->portamentoSource(1)==255,"CC121 did not clear source");
        require(player->unsupportedEvents()==unsupported,"Mono source reported unsupported");
    }
    require(sourcePitch[1]<sourcePitch[0],"CC84 did not lower initial PCM pitch");
    std::array<uint16_t,2> polyGlidePitch{};
    for(unsigned porta=0;porta<2;++porta) {
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7}); run(128000);
        send({0xc0,80}); send({0xb0,5,127}); send({0xb0,65,uint8_t(porta ? 127 : 0)});
        send({0x90,48,80}); run(512);
        send({0x90,72,80}); run(512);
        require(player->partVoiceCount(1)==4,"Poly portamento incorrectly reused a group");
        polyGlidePitch[porta]=std::max(pcm->ram2[21][0],pcm->ram2[20][0]);
    }
    require(polyGlidePitch[1]<polyGlidePitch[0],"CC65 did not use previous partial pitch");
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7}); run(128000);
    {
        const auto unsupported=player->unsupportedEvents();
        send({0xc0,80}); send({0xb0,126,1});
        send({0x90,125,80}); send({0x90,126,80}); send({0x90,127,80}); run(2048);
        require(player->freeVoices()==24 && player->currentMonoKey(1)==60,
            "Absent high-note mapping entered ordinary mono path");
        send({0xc0,24}); send({0xb0,84,48}); send({0x90,125,80}); run(512);
        require(player->partVoiceCount(1)>0 && player->currentMonoKey(1)==60
            && player->portamentoSource(1)==48,"High-note mapping consumed mono/source state");
        send({0x80,125,0}); run(128000);
        require(player->freeVoices()==24 && player->unsupportedEvents()==unsupported,
            "Mapped high note failed to release by original key");
    }
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7}); run(128000);
    send({0xc0,80}); send({0xb0,126,1}); send({0x90,60,80}); run(2048);
    require(!player->reuseInvalidated(1),"Mono switch/note did not consume invalidation");
    send({0xc0,73}); send({0xc0,80}); run(64);
    require(player->reuseInvalidated(1),"Program round trip lost invalidation history");
    send({0x90,67,80}); run(2048);
    require(!player->reuseInvalidated(1) && player->partVoiceCount(1)==2,
        "Invalidated mono note did not replace group");
    send({0xc0,80}); run(64);
    require(!player->reuseInvalidated(1),"Identical program invalidated reuse");
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7}); run(128000);
    {
        const auto unsupported=player->unsupportedEvents();
        send({0xc0,80}); send({0xb0,65,0});
        send({0x90,60,80}); send({0x90,64,80}); send({0x90,72,80}); run(4096);
        require(player->partVoiceCount(1)==6,"Poly source fixture did not start three groups");
        send({0xb0,84,64}); send({0x90,67,100}); run(2048);
        require(player->partVoiceCount(1)==6 && player->portamentoSource(1)==255,
            "Poly source did not reuse middle group");
        send({0x80,64,0}); run(128000);
        require(player->partVoiceCount(1)==6,"Old source key released renamed group");
        send({0x80,67,0}); run(128000);
        require(player->partVoiceCount(1)==4,"Destination key did not release reused group");
        send({0xb0,84,48}); send({0x90,69,90}); run(2048);
        require(player->partVoiceCount(1)==6,"Missing source did not allocate fresh group");
        send({0x80,60,0}); send({0x80,72,0}); send({0x80,69,0}); run(128000);
        require(player->freeVoices()==24 && player->unsupportedEvents()==unsupported,
            "Poly source final release/dispatch failed");
    }
    for (const uint8_t porta:{uint8_t(0),uint8_t(127)}) {
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7}); run(128000);
        const auto unsupported=player->unsupportedEvents();
        send({0xc0,80}); send({0xb0,126,1}); send({0xb0,65,porta}); send({0xb0,5,64});
        send({0x90,60,40}); run(2048);
        const auto voices=player->partVoiceCount(1);
        require(voices>0 && player->currentMonoKey(1)==60,"Mono initial note failed");
        send({0x90,67,110}); run(2048);
        send({0x90,64,80}); run(2048);
        require(player->partVoiceCount(1)==voices && player->currentMonoKey(1)==64,
            "Mono legato allocated extra voices");
        send({0x80,64,0}); run(2048);
        require(player->currentMonoKey(1)==67,"Mono return did not choose highest held key");
        send({0x90,67,0}); run(2048);
        require(player->currentMonoKey(1)==60,"Velocity-zero mono release failed");
        send({0x80,60,0}); run(128000);
        require(player->freeVoices()==24 && player->unsupportedEvents()==unsupported,"Mono release did not finish");
        send({0xc0,73}); send({0x90,60,80}); run(2048);
        send({0xc0,80}); send({0x90,67,100}); run(2048);
        require(player->currentMonoKey(1)==67 && player->partVoiceCount(1)==voices,
            "Mono program change did not replace old group");
        send({0xb0,127,1}); run(2048);
        require(player->currentMonoKey(1)==67,"Invalid poly-mode value changed mode");
        send({0xb0,127,0}); run(128000);
        require(!player->currentMonoKey(1) && player->freeVoices()==24,"Poly switch did not reclaim mono group");
    }
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7}); run(128000);
    send({0xf0,0x41,0x10,0x42,0x12,0x40,0x12,2,0,0x2c,0xf7}); run(2048);
    send({0xc0,80}); send({0xb0,126,1}); send({0xb0,65,127});
    send({0x90,60,80}); send({0x90,67,110}); run(4096);
    require(player->currentMonoKey(1)==67 && player->currentMonoKey(2)==67
        && player->partVoiceCount(1)==2 && player->partVoiceCount(2)==2,"Mono fanout note failed");
    send({0x80,67,0}); run(4096);
    require(player->currentMonoKey(1)==60 && player->currentMonoKey(2)==60,"Mono return fanout lost a part");
    send({0xb0,64,127}); send({0x80,60,0}); run(8000);
    require(player->freeVoices()==20,"Mono hold did not retain both groups");
    send({0xb0,64,0}); run(128000);
    require(player->freeVoices()==24 && player->queuedEvents()==0,"Mono hold-off/fanout did not finish");
    {
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0,0x7f,0,0x41,0xf7}); run(128000);
        const auto unsupported=player->unsupportedEvents();
        send({0xc9,8});
        //49 block2 targets the first64 key-level bytes, not tone IDs.
        std::array<uint8_t,138> bulk{0xf0,0x41,0x10,0x42,0x12,0x49,2,0};
        bulk[136]=0x35; bulk[137]=0xf7;
        require(player->push(bulk)==bulk.size(),"Rhythm bulk MIDI rejected");
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0x15,1,0x19,0xf7}); run(512);
        require(player->partSettings().parts[1].controls.program==8,
            "GS rhythm mode did not inherit shared map program");
        output.low=INT32_MAX; output.high=INT32_MIN;
        send({0x90,38,100}); run(8000);
        require(player->partVoiceCount(1)>0 && int64_t(output.high)-output.low<65536,
            "Joining shared rhythm map erased its bulk-edited key level");
        send({0xb0,120,0}); run(128000);
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0x15,2,0x18,0xf7}); run(512);
        output.low=INT32_MAX; output.high=INT32_MIN;
        send({0x90,38,100}); run(8000);
        require(int64_t(output.high)-output.low>65536,"Second rhythm map inherited first map mute");
        send({0xb0,120,0}); run(128000);
        // Assigned-controller configuration must route accepted non-default CCs.
        send({0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0x1f,19,0x7d,0xf7});
        send({0xb0,19,100}); run(512);
        require(player->unsupportedEvents()==unsupported,"Configured controller was reported unsupported");
    }
    require(mcu->pc == 0 && mcu->cycles == 0,"Native player executed H8");
    require(player->pcmBoundaryEvents()>0,"MIDI fixture never exercised PCM IRQ handoff");
    std::printf("Native MIDI PCM boundary events: %llu\n",
        static_cast<unsigned long long>(player->pcmBoundaryEvents()));
    std::printf("Native player PASS: fragmented MIDI, 3 programs, sustain/release, 24-voice saturation/reclaim, %llu real PCM frames, zero H8 cycles\n",
        static_cast<unsigned long long>(output.frames));
    return 0;
}
