#pragma once
#include "sc55_native_player.h"

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
    require(mcu->pc == 0 && mcu->cycles == 0,"Native player executed H8");
    std::printf("Native player PASS: fragmented MIDI, 3 programs, sustain/release, 24-voice saturation/reclaim, %llu real PCM frames, zero H8 cycles\n",
        static_cast<unsigned long long>(output.frames));
    return 0;
}
